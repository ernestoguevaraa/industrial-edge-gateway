// GatewayRuntime wired to the production OpcUaSource bridge (as make_gateway_runtime() does),
// with the OPC UA client and the publisher mocked: checks that the migration to the common
// acquisition API keeps the MVP 1 OPC UA -> TelemetryPoint behavior and diagnostics.

#include "ieg/acquisition/opcua_source.hpp"
#include "ieg/runtime/gateway_runtime.hpp"
#include "opcua/mock_opcua_client.hpp"
#include "output/mock_telemetry_publisher.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace ieg::runtime {
namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;

constexpr opcua::StatusCode kBadNodeIdUnknown = 0x80340000U;
constexpr opcua::StatusCode kUncertainLastUsableValue = 0x40900000U;
constexpr domain::Timestamp kSourceTime{std::chrono::sys_days{std::chrono::year{2026} / 11 / 8}};

config::GatewayConfig mvp1_config()
{
    config::GatewayConfig config;
    config.opcua = config::OpcUaConfig{.endpoint = "opc.tcp://localhost:4840",
                                       .subscription_interval = std::chrono::milliseconds{1000}};
    config.mqtt.host = "localhost";
    config.mqtt.port = 1883;
    config.mqtt.topic_prefix = "industrial";
    config.assets = {
        {.asset = "site01.line01.oven01",
         .nodes = {{.node_id = "ns=2;s=Line01.Oven01.Temperature",
                    .tag = "temperature",
                    .unit = "degC"},
                   {.node_id = "ns=2;s=Line01.Oven01.Missing", .tag = "missing", .unit = ""}}},
    };
    return config;
}

class PointSink
{
public:
    output::PublishResult publish(const domain::TelemetryPoint& point)
    {
        {
            const std::lock_guard lock(mutex_);
            points_.push_back(point);
        }
        changed_.notify_all();
        return output::PublishResult::of(output::PublishStatus::Delivered);
    }

    bool wait_for_count(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [&] { return points_.size() >= count; });
    }

    [[nodiscard]] std::vector<domain::TelemetryPoint> points() const
    {
        const std::lock_guard lock(mutex_);
        return points_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<domain::TelemetryPoint> points_;
};

class OpcUaPipeline
{
public:
    OpcUaPipeline()
    {
        auto client = std::make_unique<NiceMock<opcua::testing::MockOpcUaClient>>();
        auto publisher = std::make_unique<NiceMock<output::testing::MockTelemetryPublisher>>();
        client_ = client.get();
        publisher_ = publisher.get();
        ON_CALL(*client_, start(_, _))
            .WillByDefault(Invoke(
                [this](std::vector<opcua::MonitoredItem> items, opcua::ClientCallbacks callbacks) {
                    monitored_ = std::move(items);
                    callbacks_ = std::move(callbacks);
                }));
        ON_CALL(*publisher_, start(_))
            .WillByDefault(Invoke([this](output::PublisherStatusCallback callback) {
                publisher_callback_ = std::move(callback);
            }));
        ON_CALL(*publisher_, publish(_))
            .WillByDefault(Invoke(
                [this](const domain::TelemetryPoint& point) { return sink_.publish(point); }));
        runtime_ = std::make_unique<GatewayRuntime>(
            mvp1_config(),
            std::make_unique<acquisition::OpcUaSource>("opcua-main", std::move(client)),
            std::move(publisher),
            RuntimeOptions{
                .queue_capacity = 100, .drain_timeout = 2000ms, .report_interval = 50ms});
    }

    void start_connected()
    {
        runtime_->start();
        std::thread([this] {
            publisher_callback_({output::PublisherState::Connected, ""});
        }).join();
    }

    // Plays the open62541 worker thread.
    void fire(const opcua::DataChange& change) const
    {
        std::thread([&] { callbacks_.on_data_change(change); }).join();
    }

    GatewayRuntime& runtime() { return *runtime_; }
    opcua::testing::MockOpcUaClient& client() { return *client_; }
    output::testing::MockTelemetryPublisher& publisher() { return *publisher_; }
    PointSink& sink() { return sink_; }
    [[nodiscard]] const std::vector<opcua::MonitoredItem>& monitored() const { return monitored_; }

private:
    PointSink sink_;
    opcua::testing::MockOpcUaClient* client_{nullptr};
    output::testing::MockTelemetryPublisher* publisher_{nullptr};
    std::vector<opcua::MonitoredItem> monitored_;
    opcua::ClientCallbacks callbacks_;
    output::PublisherStatusCallback publisher_callback_;
    std::unique_ptr<GatewayRuntime> runtime_;
};

TEST(OpcUaSourceRuntime, SubscribesEveryConfiguredNodeWithConfigurationOrderIds)
{
    OpcUaPipeline pipeline;
    pipeline.start_connected();
    const std::vector<opcua::MonitoredItem> expected{{0, "ns=2;s=Line01.Oven01.Temperature"},
                                                     {1, "ns=2;s=Line01.Oven01.Missing"}};
    EXPECT_EQ(pipeline.monitored(), expected);
}

TEST(OpcUaSourceRuntime, PublishesTheSameTelemetryPointAsMvp1)
{
    OpcUaPipeline pipeline;
    pipeline.start_connected();
    const auto before =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    pipeline.fire(opcua::DataChange{.id = 0,
                                    .value = 851.25,
                                    .quality = domain::DataQuality::Uncertain,
                                    .status_code = kUncertainLastUsableValue,
                                    .source_timestamp = kSourceTime});
    ASSERT_TRUE(pipeline.sink().wait_for_count(1));
    const auto point = pipeline.sink().points().front();
    const domain::TelemetryPoint expected{.asset = "site01.line01.oven01",
                                          .tag = "temperature",
                                          .value = 851.25,
                                          .unit = "degC",
                                          .quality = domain::DataQuality::Uncertain,
                                          .source_timestamp = kSourceTime,
                                          .edge_timestamp = point.edge_timestamp,
                                          .protocol = "opcua"};
    EXPECT_EQ(point, expected);
    EXPECT_GE(point.edge_timestamp, before); // receipt time, never the source timestamp
}

TEST(OpcUaSourceRuntime, KeepsRawStatusCodeOfNoValueUpdatesInItemDiagnostics)
{
    OpcUaPipeline pipeline;
    pipeline.start_connected();
    pipeline.fire(opcua::DataChange{.id = 1,
                                    .value = std::nullopt,
                                    .quality = domain::DataQuality::Bad,
                                    .status_code = kBadNodeIdUnknown,
                                    .source_timestamp = std::nullopt});
    pipeline.fire(opcua::DataChange{.id = 99, // never requested
                                    .value = 1.0,
                                    .quality = domain::DataQuality::Good,
                                    .status_code = 0,
                                    .source_timestamp = std::nullopt});
    const auto stats = pipeline.runtime().stats();
    EXPECT_EQ(stats.no_value, 1U);
    EXPECT_EQ(stats.unknown_item, 1U);
    const auto items = pipeline.runtime().item_diagnostics();
    ASSERT_EQ(items.size(), 2U);
    EXPECT_EQ(items[1].source_id, "opcua-main");
    EXPECT_EQ(items[1].locator, "ns=2;s=Line01.Oven01.Missing");
    EXPECT_EQ(items[1].last_no_value_status,
              std::optional<acquisition::NativeStatusCode>{kBadNodeIdUnknown});
}

TEST(OpcUaSourceRuntime, ShutdownStopsTheOpcUaClientBeforeThePublisher)
{
    OpcUaPipeline pipeline;
    pipeline.start_connected();
    {
        const InSequence in_order;
        EXPECT_CALL(pipeline.client(), stop()).Times(1);
        EXPECT_CALL(pipeline.publisher(), stop()).Times(::testing::AtLeast(1));
    }
    pipeline.runtime().stop();
    // Destroying the runtime destroys OpcUaSource, whose destructor stops the client again.
    EXPECT_CALL(pipeline.client(), stop()).Times(::testing::AnyNumber());
}

} // namespace
} // namespace ieg::runtime
