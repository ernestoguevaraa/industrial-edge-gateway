#include "ieg/runtime/gateway_runtime.hpp"
#include "opcua/mock_opcua_client.hpp"
#include "output/mock_telemetry_publisher.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace ieg::runtime {
namespace {

using namespace std::chrono_literals;
using output::PublishResult;
using output::PublishStatus;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;
using Clock = std::chrono::steady_clock;

config::GatewayConfig test_config()
{
    config::GatewayConfig config;
    config.opcua.endpoint = "opc.tcp://localhost:4840";
    config.mqtt.host = "localhost";
    config.mqtt.port = 1883;
    config.mqtt.topic_prefix = "industrial";
    config.assets = {
        {.asset = "site01.line01.press01",
         .nodes = {{.node_id = "ns=2;s=Line01.Press01.Pressure", .tag = "pressure", .unit = "bar"},
                   {.node_id = "ns=2;s=Line01.Press01.State", .tag = "state", .unit = ""}}},
    };
    return config;
}

// Records what the consumer publishes (and on which thread); can block publish() until the
// mock publisher is stopped, and can return scripted results.
class PublishRecorder
{
public:
    PublishResult publish(const domain::TelemetryPoint& point)
    {
        std::unique_lock lock(mutex_);
        points_.push_back(point);
        threads_.push_back(std::this_thread::get_id());
        changed_.notify_all();
        if (blocking_) {
            changed_.wait(lock, [this] { return released_; });
            return PublishResult::of(PublishStatus::Stopped, "publisher stopped");
        }
        if (!results_.empty()) {
            auto result = std::move(results_.front());
            results_.pop_front();
            return result;
        }
        return PublishResult::of(PublishStatus::Delivered);
    }

    void release()
    {
        {
            const std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

    void set_blocking()
    {
        const std::lock_guard lock(mutex_);
        blocking_ = true;
    }

    void script(std::vector<PublishResult> results)
    {
        const std::lock_guard lock(mutex_);
        results_.assign(std::make_move_iterator(results.begin()),
                        std::make_move_iterator(results.end()));
    }

    bool wait_for_count(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return points_.size() >= count; });
    }

    [[nodiscard]] std::vector<domain::TelemetryPoint> points() const
    {
        const std::lock_guard lock(mutex_);
        return points_;
    }

    [[nodiscard]] std::vector<std::thread::id> threads() const
    {
        const std::lock_guard lock(mutex_);
        return threads_;
    }

    [[nodiscard]] std::size_t count() const
    {
        const std::lock_guard lock(mutex_);
        return points_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<domain::TelemetryPoint> points_;
    std::vector<std::thread::id> threads_;
    std::deque<PublishResult> results_;
    bool blocking_{false};
    bool released_{false};
};

opcua::DataChange change(opcua::ItemId id, double value,
                         domain::DataQuality quality = domain::DataQuality::Good)
{
    return opcua::DataChange{.id = id,
                             .value = value,
                             .quality = quality,
                             .status_code = 0,
                             .source_timestamp = std::nullopt};
}

// Owns a runtime built around mocks and plays the adapters' worker threads.
class RuntimeHarness
{
public:
    explicit RuntimeHarness(RuntimeOptions options = {.queue_capacity = 100,
                                                      .drain_timeout = 2000ms,
                                                      .report_interval = 50ms})
    {
        auto opcua = std::make_unique<NiceMock<opcua::testing::MockOpcUaClient>>();
        auto publisher = std::make_unique<NiceMock<output::testing::MockTelemetryPublisher>>();
        opcua_ = opcua.get();
        publisher_ = publisher.get();
        ON_CALL(*opcua_, start(_, _))
            .WillByDefault(Invoke(
                [this](std::vector<opcua::MonitoredItem> items, opcua::ClientCallbacks callbacks) {
                    items_ = std::move(items);
                    opcua_callbacks_ = std::move(callbacks);
                }));
        ON_CALL(*publisher_, start(_))
            .WillByDefault(Invoke([this](output::PublisherStatusCallback callback) {
                publisher_callback_ = std::move(callback);
            }));
        ON_CALL(*publisher_, publish(_))
            .WillByDefault(Invoke(
                [this](const domain::TelemetryPoint& point) { return recorder_.publish(point); }));
        ON_CALL(*publisher_, stop()).WillByDefault(Invoke([this] { recorder_.release(); }));
        runtime_ = std::make_unique<GatewayRuntime>(test_config(), std::move(opcua),
                                                    std::move(publisher), options);
    }

    // Plays the OPC UA adapter worker thread; returns how long the callback took.
    std::chrono::nanoseconds fire(const opcua::DataChange& data)
    {
        Clock::duration took{};
        std::thread adapter([&] {
            adapter_thread_ = std::this_thread::get_id();
            const auto start = Clock::now();
            opcua_callbacks_.on_data_change(data);
            took = Clock::now() - start;
        });
        adapter.join();
        return took;
    }

    void publisher_state(output::PublisherState state)
    {
        std::thread([&] { publisher_callback_({state, ""}); }).join();
    }

    GatewayRuntime& runtime() { return *runtime_; }
    PublishRecorder& recorder() { return recorder_; }
    opcua::testing::MockOpcUaClient& opcua() { return *opcua_; }
    output::testing::MockTelemetryPublisher& publisher() { return *publisher_; }
    [[nodiscard]] const std::vector<opcua::MonitoredItem>& items() const { return items_; }
    [[nodiscard]] std::thread::id adapter_thread() const { return adapter_thread_; }

private:
    PublishRecorder recorder_; // outlives the runtime (declared first)
    opcua::testing::MockOpcUaClient* opcua_{nullptr};
    output::testing::MockTelemetryPublisher* publisher_{nullptr};
    std::vector<opcua::MonitoredItem> items_;
    opcua::ClientCallbacks opcua_callbacks_;
    output::PublisherStatusCallback publisher_callback_;
    std::thread::id adapter_thread_;
    std::unique_ptr<GatewayRuntime> runtime_;
};

// Each point's edge_timestamp lies within [before, after] (taken at receipt in the callback).
::testing::AssertionResult edge_timestamps_within(const std::vector<domain::TelemetryPoint>& points,
                                                  std::chrono::system_clock::time_point before,
                                                  std::chrono::system_clock::time_point after)
{
    using std::chrono::nanoseconds;
    using std::chrono::time_point_cast;
    for (const auto& point : points) {
        if (point.edge_timestamp < time_point_cast<nanoseconds>(before) ||
            point.edge_timestamp > time_point_cast<nanoseconds>(after)) {
            return ::testing::AssertionFailure() << "edge_timestamp outside the receipt window";
        }
    }
    return ::testing::AssertionSuccess();
}

// All publishes ran on one thread that is neither the adapter thread nor the test thread.
::testing::AssertionResult on_single_consumer_thread(const std::vector<std::thread::id>& threads,
                                                     std::thread::id adapter_thread)
{
    if (threads.empty()) {
        return ::testing::AssertionFailure() << "no publish recorded";
    }
    for (const auto& thread : threads) {
        if (thread != threads.front() || thread == adapter_thread ||
            thread == std::this_thread::get_id()) {
            return ::testing::AssertionFailure() << "publish() ran on an unexpected thread";
        }
    }
    return ::testing::AssertionSuccess();
}

// ---- Startup ---------------------------------------------------------------------------------

TEST(GatewayRuntime, StartsPublisherBeforeOpcUaWithConfiguredItems)
{
    RuntimeHarness harness;
    {
        const InSequence in_order;
        EXPECT_CALL(harness.publisher(), start(_));
        EXPECT_CALL(harness.opcua(), start(_, _));
    }
    harness.runtime().start();
    EXPECT_EQ(harness.items(),
              (std::vector<opcua::MonitoredItem>{{0, "ns=2;s=Line01.Press01.Pressure"},
                                                 {1, "ns=2;s=Line01.Press01.State"}}));
    EXPECT_THROW(harness.runtime().start(), std::logic_error);
}

TEST(GatewayRuntime, StartFailureUnwindsWhatWasStarted)
{
    RuntimeHarness harness;
    EXPECT_CALL(harness.opcua(), start(_, _))
        .WillOnce(
            Invoke([](const std::vector<opcua::MonitoredItem>&, const opcua::ClientCallbacks&) {
                throw std::runtime_error("cannot start OPC UA client");
            }));
    EXPECT_CALL(harness.publisher(), stop()).Times(::testing::AtLeast(1));
    EXPECT_CALL(harness.opcua(), stop()).Times(0); // never started
    EXPECT_THROW(harness.runtime().start(), std::runtime_error);
    harness.runtime().stop(); // idempotent after the unwind
}

// ---- Data flow -------------------------------------------------------------------------------

TEST(GatewayRuntime, PublishesMappedPointsOnTheConsumerThreadOnly)
{
    RuntimeHarness harness;
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);

    const auto before = std::chrono::system_clock::now();
    harness.fire(change(0, 180.25));
    harness.fire(change(1, 1.0));
    const auto after = std::chrono::system_clock::now();
    ASSERT_TRUE(harness.recorder().wait_for_count(2, 5s));

    const auto points = harness.recorder().points();
    const auto& pressure = points.front();
    EXPECT_EQ(pressure.asset, "site01.line01.press01");
    EXPECT_EQ(pressure.tag, "pressure");
    EXPECT_EQ(pressure.unit, "bar");
    EXPECT_EQ(pressure.value, domain::TelemetryValue{180.25});
    EXPECT_EQ(pressure.protocol, "opcua");
    EXPECT_EQ(points.back().tag, "state");
    EXPECT_TRUE(edge_timestamps_within(points, before, after));
    EXPECT_TRUE(on_single_consumer_thread(harness.recorder().threads(), harness.adapter_thread()));
    EXPECT_EQ(harness.runtime().stats().delivered, 2U);
}

TEST(GatewayRuntime, KeepsBadAndUncertainQualityVisible)
{
    RuntimeHarness harness;
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    harness.fire(change(0, 1.0, domain::DataQuality::Uncertain));
    harness.fire(change(0, 2.0, domain::DataQuality::Bad));
    ASSERT_TRUE(harness.recorder().wait_for_count(2, 5s));
    const auto points = harness.recorder().points();
    EXPECT_EQ(points[0].quality, domain::DataQuality::Uncertain);
    EXPECT_EQ(points[1].quality, domain::DataQuality::Bad);
}

TEST(GatewayRuntime, NoValueChangeIsCountedWithStatusAndNeverPublished)
{
    RuntimeHarness harness;
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    constexpr opcua::StatusCode kBadNodeIdUnknown = 0x80340000U;
    harness.fire(opcua::DataChange{.id = 1,
                                   .value = std::nullopt,
                                   .quality = domain::DataQuality::Bad,
                                   .status_code = kBadNodeIdUnknown,
                                   .source_timestamp = std::nullopt});
    harness.fire(change(0, 5.0)); // a later valued update still flows
    ASSERT_TRUE(harness.recorder().wait_for_count(1, 5s));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(harness.recorder().count(), 1U);

    const auto stats = harness.runtime().stats();
    EXPECT_EQ(stats.no_value, 1U);
    const auto items = harness.runtime().item_diagnostics();
    ASSERT_EQ(items.size(), 2U);
    EXPECT_EQ(items[1].no_value, 1U);
    EXPECT_EQ(items[1].last_no_value_status, std::optional<opcua::StatusCode>{kBadNodeIdUnknown});
    EXPECT_EQ(items[0].no_value, 0U);
}

TEST(GatewayRuntime, DataCallbackNeverWaitsForPublish)
{
    RuntimeHarness harness{
        {.queue_capacity = 100, .drain_timeout = 200ms, .report_interval = 50ms}};
    harness.recorder().set_blocking(); // publish() blocks until the publisher is stopped
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    for (int i = 0; i < 50; ++i) {
        EXPECT_LT(harness.fire(change(0, i)), 50ms);
    }
    ASSERT_TRUE(harness.recorder().wait_for_count(1, 5s));
    EXPECT_EQ(harness.recorder().count(), 1U); // the consumer is stuck in publish()
    harness.runtime().stop();
}

// ---- MQTT unavailable ------------------------------------------------------------------------

TEST(GatewayRuntime, WaitsWhileDisconnectedAndBuffersTheMostRecentPoints)
{
    RuntimeHarness harness{{.queue_capacity = 3, .drain_timeout = 2000ms, .report_interval = 50ms}};
    harness.runtime().start();
    for (int i = 1; i <= 5; ++i) {
        harness.fire(change(0, i));
    }
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(harness.recorder().count(), 0U); // nothing popped while disconnected
    EXPECT_EQ(harness.runtime().stats().queue_dropped, 2U);

    harness.publisher_state(output::PublisherState::Connected);
    ASSERT_TRUE(harness.recorder().wait_for_count(3, 5s));
    const auto points = harness.recorder().points();
    EXPECT_EQ(points[0].value, domain::TelemetryValue{3.0});
    EXPECT_EQ(points[1].value, domain::TelemetryValue{4.0});
    EXPECT_EQ(points[2].value, domain::TelemetryValue{5.0});
}

TEST(GatewayRuntime, PausesWhenThePublisherDisconnects)
{
    RuntimeHarness harness;
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    harness.fire(change(0, 1.0));
    ASSERT_TRUE(harness.recorder().wait_for_count(1, 5s));

    harness.publisher_state(output::PublisherState::Disconnected);
    harness.fire(change(0, 2.0));
    std::this_thread::sleep_for(300ms); // longer than one consumer pop wait
    EXPECT_EQ(harness.recorder().count(), 1U);

    harness.publisher_state(output::PublisherState::Connected);
    ASSERT_TRUE(harness.recorder().wait_for_count(2, 5s));
    EXPECT_EQ(harness.recorder().points()[1].value, domain::TelemetryValue{2.0});
}

TEST(GatewayRuntime, FailedPublishesAreCountedAndNotRetried)
{
    RuntimeHarness harness;
    harness.recorder().script({PublishResult::of(PublishStatus::NotConnected),
                               PublishResult::of(PublishStatus::Unavailable),
                               PublishResult::of(PublishStatus::Timeout),
                               PublishResult::rejected(output::RetryDisposition::Permanent, "acl"),
                               PublishResult::of(PublishStatus::InvalidPoint),
                               PublishResult::of(PublishStatus::Delivered)});
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    for (int i = 0; i < 6; ++i) {
        harness.fire(change(0, i));
    }
    ASSERT_TRUE(harness.recorder().wait_for_count(6, 5s));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(harness.recorder().count(), 6U); // exactly one attempt per point

    const auto stats = harness.runtime().stats();
    const std::vector<std::uint64_t> counts{stats.not_connected, stats.unavailable,
                                            stats.timeout,       stats.rejected,
                                            stats.invalid_point, stats.delivered};
    EXPECT_EQ(counts, std::vector<std::uint64_t>(6, 1U)); // one of each outcome
}

// ---- Shutdown --------------------------------------------------------------------------------

TEST(GatewayRuntime, ShutdownStopsProducersFirstThenDrainsThenStopsPublisher)
{
    RuntimeHarness harness;
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    for (int i = 0; i < 5; ++i) {
        harness.fire(change(0, i));
    }
    std::size_t published_when_publisher_stopped = 0;
    {
        const InSequence in_order;
        EXPECT_CALL(harness.opcua(), stop());
        EXPECT_CALL(harness.publisher(), stop()).WillOnce(Invoke([&] {
            published_when_publisher_stopped = harness.recorder().count();
            harness.recorder().release();
        }));
    }
    harness.runtime().stop();
    EXPECT_EQ(published_when_publisher_stopped, 5U); // queued points drained first
    EXPECT_EQ(harness.runtime().stats().shutdown_dropped, 0U);
}

TEST(GatewayRuntime, DrainDeadlineInterruptsBlockedPublishAndCountsLeftovers)
{
    RuntimeHarness harness{
        {.queue_capacity = 100, .drain_timeout = 300ms, .report_interval = 50ms}};
    harness.recorder().set_blocking();
    harness.runtime().start();
    harness.publisher_state(output::PublisherState::Connected);
    for (int i = 0; i < 3; ++i) {
        harness.fire(change(0, i));
    }
    ASSERT_TRUE(harness.recorder().wait_for_count(1, 5s)); // first point blocked in publish()

    const auto before = Clock::now();
    harness.runtime().stop();
    const auto took = Clock::now() - before;
    EXPECT_GE(took, 300ms);
    EXPECT_LT(took, 2s);

    const auto stats = harness.runtime().stats();
    EXPECT_EQ(stats.stopped_in_flight, 1U); // the point whose publish() returned Stopped
    EXPECT_EQ(stats.shutdown_dropped, 2U);  // never attempted
    EXPECT_EQ(stats.delivered, 0U);
}

TEST(GatewayRuntime, ShutdownWhileDisconnectedDoesNotWaitForDrain)
{
    RuntimeHarness harness{
        {.queue_capacity = 100, .drain_timeout = 5000ms, .report_interval = 50ms}};
    harness.runtime().start();
    for (int i = 0; i < 3; ++i) {
        harness.fire(change(0, i));
    }
    const auto before = Clock::now();
    harness.runtime().stop();
    EXPECT_LT(Clock::now() - before, 1s);
    EXPECT_EQ(harness.recorder().count(), 0U);
    EXPECT_EQ(harness.runtime().stats().shutdown_dropped, 3U);
}

TEST(GatewayRuntime, StopIsIdempotentAndSafeBeforeStart)
{
    RuntimeHarness never_started;
    never_started.runtime().stop();

    RuntimeHarness harness;
    EXPECT_CALL(harness.opcua(), stop()).Times(1);
    harness.runtime().start();
    harness.runtime().stop();
    harness.runtime().stop();
}

} // namespace
} // namespace ieg::runtime
