// GatewayRuntime with several acquisition sources (OPC UA + two Modbus TCP devices, and
// Modbus-only): routing by (source_id, item_id), unknown sources, partial start failures,
// shutdown order and concurrency, concurrent callbacks and per-item diagnostics.

#include "acquisition/mock_acquisition_source.hpp"
#include "ieg/runtime/gateway_runtime.hpp"
#include "output/mock_telemetry_publisher.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ieg::runtime {
namespace {

using namespace std::chrono_literals;
using acquisition::testing::MockAcquisitionSource;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using Clock = std::chrono::steady_clock;

constexpr const char* kOpcUa = "opcua-main";
constexpr const char* kPress = "press-plc";
constexpr const char* kOven = "oven-plc";

config::ModbusPointConfig modbus_point(const std::string& asset, const std::string& tag,
                                       std::uint16_t offset)
{
    config::ModbusPointConfig point;
    point.asset = asset;
    point.tag = tag;
    point.unit = "bar";
    point.table = config::ModbusTable::HoldingRegisters;
    point.offset = offset;
    point.type = config::ModbusDataType::UInt16;
    return point;
}

config::ModbusDeviceConfig modbus_device(const std::string& id,
                                         std::vector<config::ModbusPointConfig> points)
{
    config::ModbusDeviceConfig device;
    device.source_id = id;
    device.host = id + ".local";
    device.port = 502;
    device.unit_id = 1;
    device.points = std::move(points);
    return device;
}

// OPC UA: 2 nodes; press-plc: 2 points; oven-plc: 1 point.
config::GatewayConfig mixed_config()
{
    config::GatewayConfig config;
    config.opcua = config::OpcUaConfig{.endpoint = "opc.tcp://localhost:4840",
                                       .subscription_interval = 1000ms};
    config.mqtt.host = "localhost";
    config.mqtt.port = 1883;
    config.mqtt.topic_prefix = "industrial";
    config.assets = {
        {.asset = "site01.line01.press01",
         .nodes = {{.node_id = "ns=2;s=Press01.Pressure", .tag = "pressure", .unit = "bar"},
                   {.node_id = "ns=2;s=Press01.State", .tag = "state", .unit = ""}}},
    };
    config.modbus.devices = {
        modbus_device(kPress, {modbus_point("site01.line01.press02", "pressure", 0),
                               modbus_point("site01.line01.press02", "state", 1)}),
        modbus_device(kOven, {modbus_point("site01.line01.oven02", "temperature", 0)}),
    };
    return config;
}

config::GatewayConfig modbus_only_config()
{
    auto config = mixed_config();
    config.opcua.reset();
    config.assets.clear();
    config.modbus.devices.resize(1);
    return config;
}

acquisition::Update update(const std::string& source, acquisition::ItemId item, double value)
{
    return acquisition::Update{.source_id = source,
                               .item_id = item,
                               .value = value,
                               .quality = domain::DataQuality::Good,
                               .source_timestamp = std::nullopt,
                               .native_status = std::nullopt,
                               .diagnostic = {}};
}

acquisition::Update bad(const std::string& source, acquisition::ItemId item,
                        std::optional<acquisition::NativeStatusCode> native, std::string diagnostic)
{
    return acquisition::Update{.source_id = source,
                               .item_id = item,
                               .value = std::nullopt,
                               .quality = domain::DataQuality::Bad,
                               .source_timestamp = std::nullopt,
                               .native_status = native,
                               .diagnostic = std::move(diagnostic)};
}

// Thread-safe record of published points and of lifecycle calls, in order.
class Journal
{
public:
    void publish(const domain::TelemetryPoint& point)
    {
        {
            const std::lock_guard lock(mutex_);
            points_.push_back(point);
        }
        changed_.notify_all();
    }

    void note(const std::string& event)
    {
        const std::lock_guard lock(mutex_);
        events_.push_back(event);
    }

    bool wait_for_points(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] { return points_.size() >= count; });
    }

    [[nodiscard]] std::vector<domain::TelemetryPoint> points() const
    {
        const std::lock_guard lock(mutex_);
        return points_;
    }

    [[nodiscard]] std::vector<std::string> events() const
    {
        const std::lock_guard lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<domain::TelemetryPoint> points_;
    std::vector<std::string> events_;
};

// Owns a runtime around one mock source per configured source and a mock publisher.
class MultiSourceHarness
{
public:
    explicit MultiSourceHarness(const config::GatewayConfig& config,
                                std::chrono::milliseconds stop_delay = 0ms)
    {
        std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources;
        std::vector<std::string> ids;
        if (config.opcua) {
            ids.emplace_back(kOpcUa);
        }
        for (const auto& device : config.modbus.devices) {
            ids.push_back(device.source_id);
        }
        for (const auto& id : ids) {
            auto source = std::make_unique<NiceMock<MockAcquisitionSource>>(id);
            ON_CALL(*source, start(_, _))
                .WillByDefault(Invoke([this, id](std::vector<acquisition::Item> items,
                                                 acquisition::Callbacks callbacks) {
                    const std::lock_guard lock(mutex_);
                    items_[id] = std::move(items);
                    callbacks_[id] = std::move(callbacks);
                    journal_.note("start " + id);
                }));
            ON_CALL(*source, stop()).WillByDefault(Invoke([this, id, stop_delay] {
                std::this_thread::sleep_for(stop_delay);
                journal_.note("stop " + id);
            }));
            mocks_[id] = source.get();
            sources.push_back(std::move(source));
        }
        auto publisher = std::make_unique<NiceMock<output::testing::MockTelemetryPublisher>>();
        publisher_ = publisher.get();
        ON_CALL(*publisher_, start(_))
            .WillByDefault(Invoke([this](output::PublisherStatusCallback callback) {
                publisher_callback_ = std::move(callback);
                journal_.note("start publisher");
            }));
        ON_CALL(*publisher_, publish(_))
            .WillByDefault(Invoke([this](const domain::TelemetryPoint& point) {
                journal_.publish(point);
                return output::PublishResult::of(output::PublishStatus::Delivered);
            }));
        ON_CALL(*publisher_, stop()).WillByDefault(Invoke([this] {
            journal_.note("stop publisher");
        }));
        runtime_ = std::make_unique<GatewayRuntime>(
            config, std::move(sources), std::move(publisher),
            RuntimeOptions{
                .queue_capacity = 100'000, .drain_timeout = 2000ms, .report_interval = 50ms});
    }

    void start_connected()
    {
        runtime_->start();
        std::thread([this] {
            publisher_callback_({output::PublisherState::Connected, ""});
        }).join();
    }

    // Plays the given source's worker thread.
    void fire(const acquisition::Update& data)
    {
        const auto callbacks = callbacks_for(data.source_id);
        std::thread([&] { callbacks.on_update(data); }).join();
    }

    [[nodiscard]] acquisition::Callbacks callbacks_for(const std::string& id)
    {
        const std::lock_guard lock(mutex_);
        return callbacks_.at(id);
    }

    [[nodiscard]] std::vector<acquisition::Item> items_of(const std::string& id)
    {
        const std::lock_guard lock(mutex_);
        return items_.at(id);
    }

    GatewayRuntime& runtime() { return *runtime_; }
    MockAcquisitionSource& source(const std::string& id) { return *mocks_.at(id); }
    output::testing::MockTelemetryPublisher& publisher() { return *publisher_; }
    Journal& journal() { return journal_; }

private:
    Journal journal_; // outlives the runtime (declared first)
    std::mutex mutex_;
    std::map<std::string, std::vector<acquisition::Item>> items_;
    std::map<std::string, acquisition::Callbacks> callbacks_;
    std::map<std::string, MockAcquisitionSource*> mocks_;
    output::testing::MockTelemetryPublisher* publisher_{nullptr};
    output::PublisherStatusCallback publisher_callback_;
    std::unique_ptr<GatewayRuntime> runtime_;
};

bool construction_rejected(const config::GatewayConfig& config, const std::vector<std::string>& ids,
                           bool with_null = false)
{
    std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources;
    sources.reserve(ids.size() + 1);
    for (const auto& id : ids) {
        sources.push_back(std::make_unique<NiceMock<MockAcquisitionSource>>(id));
    }
    if (with_null) {
        sources.push_back(nullptr);
    }
    try {
        const GatewayRuntime runtime{
            config, std::move(sources),
            std::make_unique<NiceMock<output::testing::MockTelemetryPublisher>>(),
            RuntimeOptions{}};
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

std::size_t index_of(const std::vector<std::string>& events, const std::string& event)
{
    return static_cast<std::size_t>(std::ranges::find(events, event) - events.begin());
}

// Whether start() throws std::runtime_error.
bool start_fails(GatewayRuntime& runtime)
{
    try {
        runtime.start();
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

// Every source in `ids` was stopped, and before the publisher (which was stopped too).
::testing::AssertionResult stopped_before_publisher(const std::vector<std::string>& events,
                                                    const std::vector<std::string>& ids)
{
    const auto publisher = index_of(events, "stop publisher");
    if (publisher == events.size()) {
        return ::testing::AssertionFailure() << "the publisher was not stopped";
    }
    for (const auto& id : ids) {
        if (index_of(events, "stop " + id) >= publisher) {
            return ::testing::AssertionFailure() << id << " was not stopped before the publisher";
        }
    }
    return ::testing::AssertionSuccess();
}

// Plays `per_source` updates (plus a Bad update and a status every 100) from every source on its
// own thread, all concurrently; returns when every thread has finished.
void fire_concurrently(MultiSourceHarness& harness, const std::vector<std::string>& ids,
                       int per_source)
{
    std::vector<std::jthread> workers;
    workers.reserve(ids.size());
    for (const auto& id : ids) {
        workers.emplace_back([callbacks = harness.callbacks_for(id), id, per_source] {
            for (int i = 0; i < per_source; ++i) {
                callbacks.on_update(update(id, 0, static_cast<double>(i)));
                if (i % 100 == 0) {
                    callbacks.on_update(bad(id, 0, std::nullopt, "timeout"));
                    callbacks.on_status(
                        acquisition::Status{.source_id = id,
                                            .state = acquisition::ConnectionState::Connected,
                                            .detail = ""});
                }
            }
        });
    }
}

// ---- Construction --------------------------------------------------------------------------

TEST(MultiSourceRuntime, RequiresExactlyTheConfiguredSourcesInAnyOrder)
{
    const auto config = mixed_config();
    EXPECT_FALSE(construction_rejected(config, {kOven, kOpcUa, kPress}));
    EXPECT_TRUE(construction_rejected(config, {kOpcUa, kPress}));                 // oven missing
    EXPECT_TRUE(construction_rejected(config, {kOpcUa, kPress, kOven, "ghost"})); // unexpected
    EXPECT_TRUE(construction_rejected(config, {kOpcUa, kPress, kPress, kOven}));  // duplicate
    EXPECT_TRUE(construction_rejected(config, {kOpcUa, kPress, kOven}, true));    // null
}

// ---- Start -----------------------------------------------------------------------------------

TEST(MultiSourceRuntime, StartsThePublisherFirstAndEverySourceWithItsOwnItems)
{
    MultiSourceHarness harness{mixed_config()};
    harness.runtime().start();
    const auto events = harness.journal().events();
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0], "start publisher");
    EXPECT_EQ(harness.items_of(kOpcUa),
              (std::vector<acquisition::Item>{{0, "ns=2;s=Press01.Pressure"},
                                              {1, "ns=2;s=Press01.State"}}));
    EXPECT_EQ(harness.items_of(kPress),
              (std::vector<acquisition::Item>{{0, "site01.line01.press02/pressure"},
                                              {1, "site01.line01.press02/state"}}));
    EXPECT_EQ(harness.items_of(kOven),
              (std::vector<acquisition::Item>{{0, "site01.line01.oven02/temperature"}}));
}

TEST(MultiSourceRuntime, PartialStartFailureStopsEveryAttemptedSourceThenThePublisher)
{
    MultiSourceHarness harness{mixed_config()};
    EXPECT_CALL(harness.source(kPress), start(_, _))
        .WillOnce(Invoke([](const std::vector<acquisition::Item>&, const acquisition::Callbacks&) {
            throw std::runtime_error("cannot start press-plc");
        }));
    EXPECT_CALL(harness.source(kOven), start(_, _)).Times(0); // never attempted
    EXPECT_CALL(harness.source(kOven), stop()).Times(0);
    EXPECT_TRUE(start_fails(harness.runtime()));
    // opcua-main started; press-plc's start threw: both stopped before the publisher.
    EXPECT_TRUE(stopped_before_publisher(harness.journal().events(), {kOpcUa, kPress}));
    harness.runtime().stop(); // idempotent after the unwind
}

// ---- Data routing ----------------------------------------------------------------------------

TEST(MultiSourceRuntime, EqualItemIdsFromDifferentSourcesBecomeDifferentPoints)
{
    MultiSourceHarness harness{mixed_config()};
    harness.start_connected();
    harness.fire(update(kOpcUa, 0, 1.0));
    harness.fire(update(kPress, 0, 2.0));
    harness.fire(update(kOven, 0, 3.0));
    ASSERT_TRUE(harness.journal().wait_for_points(3));
    const auto points = harness.journal().points();
    EXPECT_EQ(points[0].asset, "site01.line01.press01");
    EXPECT_EQ(points[0].protocol, "opcua");
    EXPECT_EQ(points[1].asset, "site01.line01.press02");
    EXPECT_EQ(points[1].tag, "pressure");
    EXPECT_EQ(points[1].protocol, "modbus_tcp");
    EXPECT_FALSE(points[1].source_timestamp.has_value()); // never invented for Modbus
    EXPECT_EQ(points[2].asset, "site01.line01.oven02");
    EXPECT_EQ(points[2].value, domain::TelemetryValue{3.0});
}

TEST(MultiSourceRuntime, UpdatesAndStatusesFromAnUnknownSourceAreDropped)
{
    MultiSourceHarness harness{mixed_config()};
    harness.start_connected();
    const auto callbacks = harness.callbacks_for(kPress); // any registered callback
    std::thread([&] {
        callbacks.on_update(update("ghost-plc", 0, 9.0));
        callbacks.on_status(acquisition::Status{.source_id = "ghost-plc",
                                                .state = acquisition::ConnectionState::Connected,
                                                .detail = ""});
    }).join();
    harness.fire(update(kPress, 7, 1.0)); // unknown item of a known source
    harness.fire(update(kPress, 1, 4.0));
    ASSERT_TRUE(harness.journal().wait_for_points(1));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(harness.journal().points().size(), 1U);
    const auto stats = harness.runtime().stats();
    EXPECT_EQ(stats.unknown_source, 2U);
    EXPECT_EQ(stats.unknown_item, 1U);
}

TEST(MultiSourceRuntime, ModbusOnlyConfigurationPublishesModbusPoints)
{
    MultiSourceHarness harness{modbus_only_config()};
    harness.start_connected();
    harness.fire(update(kPress, 1, 5.0));
    ASSERT_TRUE(harness.journal().wait_for_points(1));
    const auto point = harness.journal().points().front();
    EXPECT_EQ(point.asset, "site01.line01.press02");
    EXPECT_EQ(point.tag, "state");
    EXPECT_EQ(point.protocol, "modbus_tcp");
}

// ---- Diagnostics -----------------------------------------------------------------------------

TEST(MultiSourceRuntime, KeepsDiagnosticsPerSourceAndItemWithoutMixingProtocols)
{
    MultiSourceHarness harness{mixed_config()};
    harness.start_connected();
    harness.fire(bad(kOpcUa, 0, 0x80340000U, ""));           // OPC UA BadNodeIdUnknown
    harness.fire(bad(kPress, 0, 0x02U, "modbus_exception")); // Modbus exception 0x02
    harness.fire(bad(kOven, 0, std::nullopt, "timeout"));    // no native status
    const auto items = harness.runtime().item_diagnostics();
    ASSERT_EQ(items.size(), 5U);
    EXPECT_EQ(items[0].source_id, kOpcUa);
    EXPECT_EQ(items[0].protocol, "opcua");
    EXPECT_EQ(items[0].last_no_value_status,
              std::optional<acquisition::NativeStatusCode>{0x80340000U});
    EXPECT_EQ(items[2].source_id, kPress);
    EXPECT_EQ(items[2].protocol, "modbus_tcp");
    EXPECT_EQ(items[2].locator, "site01.line01.press02/pressure");
    EXPECT_EQ(items[2].last_no_value_status, std::optional<acquisition::NativeStatusCode>{0x02U});
    EXPECT_EQ(items[2].last_no_value_diagnostic, "modbus_exception");
    EXPECT_EQ(items[4].source_id, kOven);
    EXPECT_EQ(items[4].last_no_value_status, std::nullopt);
    EXPECT_EQ(items[4].last_no_value_diagnostic, "timeout");
    EXPECT_EQ(items[3].no_value, 0U); // press-plc item 1 untouched
    EXPECT_EQ(harness.runtime().stats().no_value, 3U);
}

// ---- Concurrency -----------------------------------------------------------------------------

TEST(MultiSourceRuntime, CallbacksOfDifferentSourcesRunConcurrentlyWithoutLosingUpdates)
{
    constexpr int kPerSource = 3000;
    MultiSourceHarness harness{mixed_config()};
    harness.runtime().start(); // publisher stays disconnected: points remain queued
    fire_concurrently(harness, {kOpcUa, kPress, kOven}, kPerSource);
    const auto stats = harness.runtime().stats();
    EXPECT_EQ(stats.received, 3U * (kPerSource + kPerSource / 100));
    EXPECT_EQ(stats.enqueued, 3U * kPerSource);
    EXPECT_EQ(stats.no_value, 3U * (kPerSource / 100));
    EXPECT_EQ(stats.queue_dropped, 0U);
    EXPECT_EQ(stats.unknown_item + stats.unknown_source, 0U);
}

// ---- Shutdown --------------------------------------------------------------------------------

TEST(MultiSourceRuntime, ShutdownStopsEverySourceBeforeThePublisher)
{
    MultiSourceHarness harness{mixed_config()};
    harness.start_connected();
    harness.runtime().stop();
    EXPECT_TRUE(stopped_before_publisher(harness.journal().events(), {kOpcUa, kPress, kOven}));
    harness.runtime().stop(); // idempotent
}

TEST(MultiSourceRuntime, StopsSourcesConcurrentlySoShutdownWaitsForTheSlowestOnly)
{
    MultiSourceHarness harness{mixed_config(), 300ms}; // every source needs 300 ms to stop
    harness.start_connected();
    const auto before = Clock::now();
    harness.runtime().stop();
    const auto took = Clock::now() - before;
    EXPECT_GE(took, 300ms);
    EXPECT_LT(took, 800ms); // sequential stopping would take at least 900 ms
}

} // namespace
} // namespace ieg::runtime
