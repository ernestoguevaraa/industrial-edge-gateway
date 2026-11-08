#include "ieg/config/config_loader.hpp"
#include "ieg/runtime/gateway_runtime.hpp"
#include "runtime/telemetry_mapping.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ieg::runtime {
namespace {

using detail::ItemTable;
using detail::make_point;
using ::testing::HasSubstr;

config::GatewayConfig two_asset_config()
{
    config::GatewayConfig config;
    config.opcua = config::OpcUaConfig{.endpoint = "opc.tcp://localhost:4840",
                                       .subscription_interval = std::chrono::milliseconds{1000}};
    config.mqtt.host = "localhost";
    config.mqtt.port = 1883;
    config.mqtt.topic_prefix = "industrial";
    config.assets = {
        {.asset = "site01.line01.press01",
         .nodes = {{.node_id = "ns=2;s=Line01.Press01.Pressure", .tag = "pressure", .unit = "bar"},
                   {.node_id = "ns=2;s=Line01.Press01.State", .tag = "state", .unit = ""}}},
        {.asset = "site01.line01.oven01",
         .nodes = {{.node_id = "ns=2;s=Line01.Oven01.Temperature",
                    .tag = "temperature",
                    .unit = "degC"}}},
    };
    return config;
}

constexpr domain::Timestamp kSourceTime{std::chrono::sys_days{std::chrono::year{2026} / 10 / 17}};
constexpr domain::Timestamp kEdgeTime = kSourceTime + std::chrono::milliseconds{7};

// ---- ItemTable ---------------------------------------------------------------------------

TEST(ItemTable, AssignsIdsInConfigurationOrder)
{
    const ItemTable table{two_asset_config()};
    ASSERT_EQ(table.size(), 3U);
    const std::vector<acquisition::Item> expected{{0, "ns=2;s=Line01.Press01.Pressure"},
                                                  {1, "ns=2;s=Line01.Press01.State"},
                                                  {2, "ns=2;s=Line01.Oven01.Temperature"}};
    EXPECT_EQ(table.items(), expected);

    const auto* oven = table.find(2);
    ASSERT_NE(oven, nullptr);
    EXPECT_EQ(oven->asset, "site01.line01.oven01");
    EXPECT_EQ(oven->tag, "temperature");
    EXPECT_EQ(oven->unit, "degC");
    EXPECT_EQ(oven->locator, "ns=2;s=Line01.Oven01.Temperature");
    EXPECT_EQ(oven->protocol, "opcua");
    EXPECT_EQ(table.find(3), nullptr);
}

TEST(ItemTable, RejectsConfigurationWithoutNodes)
{
    auto config = two_asset_config();
    config.assets.clear();
    EXPECT_THROW(ItemTable{config}, std::invalid_argument);
}

// ---- make_point ----------------------------------------------------------------------------

TEST(MakePoint, MapsValueMetadataAndTimestamps)
{
    const ItemTable table{two_asset_config()};
    const acquisition::Update change{.source_id = "opcua-main",
                                     .item_id = 0,
                                     .value = 180.5,
                                     .quality = domain::DataQuality::Good,
                                     .source_timestamp = kSourceTime,
                                     .native_status = 0,
                                     .diagnostic = {}};
    const auto point = make_point(*table.find(0), change, kEdgeTime);
    const std::optional<domain::TelemetryPoint> expected{
        domain::TelemetryPoint{.asset = "site01.line01.press01",
                               .tag = "pressure",
                               .value = 180.5,
                               .unit = "bar",
                               .quality = domain::DataQuality::Good,
                               .source_timestamp = kSourceTime,
                               .edge_timestamp = kEdgeTime,
                               .protocol = "opcua"}};
    EXPECT_EQ(point, expected);
}

TEST(MakePoint, KeepsUncertainAndBadQualityVisible)
{
    const ItemTable table{two_asset_config()};
    for (const auto quality : {domain::DataQuality::Uncertain, domain::DataQuality::Bad}) {
        const acquisition::Update change{.source_id = "opcua-main",
                                         .item_id = 2,
                                         .value = 851.0,
                                         .quality = quality,
                                         .source_timestamp = kSourceTime,
                                         .native_status = 0x808C0000U,
                                         .diagnostic = {}};
        const auto point = make_point(*table.find(2), change, kEdgeTime);
        EXPECT_TRUE(point.has_value() && point->quality == quality);
    }
}

TEST(MakePoint, AbsentSourceTimestampStaysAbsent)
{
    const ItemTable table{two_asset_config()};
    const acquisition::Update change{.source_id = "opcua-main",
                                     .item_id = 1,
                                     .value = std::string{"RUNNING"},
                                     .quality = domain::DataQuality::Good,
                                     .source_timestamp = std::nullopt,
                                     .native_status = 0,
                                     .diagnostic = {}};
    const auto point = make_point(*table.find(1), change, kEdgeTime);
    EXPECT_TRUE(point.has_value() && !point->source_timestamp.has_value() &&
                point->edge_timestamp == kEdgeTime);
}

TEST(MakePoint, NoValueNeverBecomesATelemetryPoint)
{
    const ItemTable table{two_asset_config()};
    const acquisition::Update change{.source_id = "opcua-main",
                                     .item_id = 0,
                                     .value = std::nullopt,
                                     .quality = domain::DataQuality::Bad,
                                     .source_timestamp = std::nullopt,
                                     .native_status = 0x80340000U,
                                     .diagnostic = {}};
    EXPECT_FALSE(make_point(*table.find(0), change, kEdgeTime).has_value());
}

TEST(ReceiptTime, IsCurrentSystemTime)
{
    const auto before = std::chrono::system_clock::now();
    const auto receipt = detail::receipt_time();
    const auto after = std::chrono::system_clock::now();
    EXPECT_GE(receipt, std::chrono::time_point_cast<std::chrono::nanoseconds>(before));
    EXPECT_LE(receipt, std::chrono::time_point_cast<std::chrono::nanoseconds>(after));
}

config::ModbusDeviceConfig modbus_device()
{
    return config::ModbusDeviceConfig{
        .source_id = "press-plc",
        .host = "modbus-simulator",
        .port = 502,
        .unit_id = 1,
        .poll_interval = std::chrono::milliseconds{1000},
        .timeout = std::chrono::milliseconds{500},
        .points = {config::ModbusPointConfig{.asset = "site01.line01.press02",
                                             .tag = "pressure",
                                             .unit = "bar",
                                             .table = config::ModbusTable::HoldingRegisters,
                                             .offset = 0,
                                             .type = config::ModbusDataType::UInt16,
                                             .word_order = std::nullopt,
                                             .scale = 0.1}}};
}

// ---- Startup validation -------------------------------------------------------------------

TEST(ValidateForMvp1, AcceptsQos1)
{
    auto config = two_asset_config();
    config.mqtt.qos = config::MqttQos::AtLeastOnce;
    EXPECT_NO_THROW(validate_for_mvp1(config));
}

TEST(ValidateForMvp1, RejectsQos0And2Clearly)
{
    for (const auto qos : {config::MqttQos::AtMostOnce, config::MqttQos::ExactlyOnce}) {
        auto config = two_asset_config();
        config.mqtt.qos = qos;
        try {
            validate_for_mvp1(config);
            ADD_FAILURE() << "expected StartupError";
        } catch (const StartupError& error) {
            EXPECT_THAT(error.what(), HasSubstr("mqtt.qos must be 1"));
            EXPECT_THAT(error.what(), HasSubstr("got " + std::to_string(static_cast<int>(qos))));
        }
    }
}

TEST(ValidateForMvp1, RejectsMixedConfigurationBecauseModbusIsNotImplemented)
{
    auto config = two_asset_config();
    config.modbus.devices.push_back(modbus_device());
    try {
        validate_for_mvp1(config);
        ADD_FAILURE() << "expected StartupError";
    } catch (const StartupError& error) {
        EXPECT_THAT(error.what(), HasSubstr("Modbus TCP acquisition is not implemented yet"));
    }
}

TEST(ValidateForMvp1, RejectsModbusOnlyConfiguration)
{
    auto config = two_asset_config();
    config.opcua.reset();
    config.assets.clear();
    config.modbus.devices.push_back(modbus_device());
    try {
        validate_for_mvp1(config);
        ADD_FAILURE() << "expected StartupError";
    } catch (const StartupError& error) {
        EXPECT_THAT(error.what(), HasSubstr("Modbus TCP acquisition is not implemented yet"));
        EXPECT_THAT(error.what(), HasSubstr("an OPC UA source (opcua and assets) is required"));
    }
}

TEST(MakeGatewayRuntime, RejectsTheMixedExampleFileAtStartup)
{
    // The mixed example parses and validates, but must not start as if Modbus worked.
    const auto config = config::load_config_file(IEG_MIXED_EXAMPLE_CONFIG_PATH);
    ASSERT_FALSE(config.modbus.devices.empty());
    EXPECT_THROW(static_cast<void>(make_gateway_runtime(config)), StartupError);
}

TEST(RuntimeOptionsFrom, UsesConfiguredQueueCapacityAndDrainTimeout)
{
    auto config = two_asset_config();
    config.runtime.queue_capacity = 42;
    config.runtime.shutdown_drain_timeout = std::chrono::milliseconds{1234};
    const auto options = runtime_options_from(config);
    EXPECT_EQ(options.queue_capacity, 42U);
    EXPECT_EQ(options.drain_timeout, std::chrono::milliseconds{1234});
}

TEST(MakeGatewayRuntime, RejectsQosOtherThan1BeforeBuildingAdapters)
{
    auto config = two_asset_config();
    config.mqtt.qos = config::MqttQos::AtMostOnce;
    EXPECT_THROW(static_cast<void>(make_gateway_runtime(config)), StartupError);
}

TEST(MakeGatewayRuntime, BuildsRuntimeWithoutNetworkActivity)
{
    auto config = two_asset_config();
    config.mqtt.client_id = "ieg-unit-test";
    const auto runtime = make_gateway_runtime(config);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->stats().queue.capacity, config.runtime.queue_capacity);
}

} // namespace
} // namespace ieg::runtime
