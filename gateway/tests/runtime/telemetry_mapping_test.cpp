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

// The OPC UA source ID as the std::string the item table is keyed by.
const std::string& opc()
{
    static const std::string id{detail::kOpcUaSourceId};
    return id;
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
    ASSERT_EQ(table.sources().size(), 1U);
    EXPECT_EQ(table.sources()[0].source_id, "opcua-main");
    EXPECT_EQ(table.sources()[0].protocol, "opcua");
    EXPECT_EQ(table.sources()[0].target, "opc.tcp://localhost:4840");
    EXPECT_EQ(table.sources()[0].items, expected);

    const auto* oven = table.find(opc(), 2);
    ASSERT_NE(oven, nullptr);
    EXPECT_EQ(oven->asset, "site01.line01.oven01");
    EXPECT_EQ(oven->tag, "temperature");
    EXPECT_EQ(oven->unit, "degC");
    EXPECT_EQ(oven->locator, "ns=2;s=Line01.Oven01.Temperature");
    EXPECT_EQ(oven->protocol, "opcua");
    EXPECT_EQ(table.find(opc(), 3), nullptr);
}

TEST(ItemTable, RejectsConfigurationWithoutNodes)
{
    auto config = two_asset_config();
    config.assets.clear();
    EXPECT_THROW(ItemTable{config}, std::invalid_argument);
}

TEST(ItemTable, AddsOneSourcePerModbusDeviceAfterOpcUa)
{
    auto config = two_asset_config();
    config.modbus.devices.push_back(modbus_device());
    const ItemTable table{config};
    ASSERT_EQ(table.sources().size(), 2U);
    const auto& modbus = table.sources()[1];
    EXPECT_EQ(modbus.source_id, "press-plc");
    EXPECT_EQ(modbus.protocol, "modbus_tcp");
    EXPECT_EQ(modbus.target, "modbus-simulator:502");
    EXPECT_EQ(modbus.items,
              (std::vector<acquisition::Item>{{0, "site01.line01.press02/pressure"}}));
    EXPECT_EQ(table.size(), 4U);
    EXPECT_EQ(table.source_index("press-plc"), std::optional<std::size_t>{1});
}

TEST(ItemTable, KeysItemsBySourceSoEqualItemIdsNeverCollide)
{
    auto config = two_asset_config();
    config.modbus.devices.push_back(modbus_device());
    const ItemTable table{config};
    const auto* opcua = table.find(opc(), 0);
    const auto* modbus = table.find("press-plc", 0);
    ASSERT_NE(opcua, nullptr);
    ASSERT_NE(modbus, nullptr);
    EXPECT_NE(opcua, modbus);
    EXPECT_EQ(opcua->tag, "pressure");
    EXPECT_EQ(opcua->asset, "site01.line01.press01");
    EXPECT_EQ(modbus->asset, "site01.line01.press02");
    EXPECT_EQ(modbus->protocol, "modbus_tcp");
    EXPECT_EQ(modbus->unit, "bar");
    EXPECT_NE(table.index_of(*opcua), table.index_of(*modbus));
}

TEST(ItemTable, ReturnsNothingForUnknownSourcesOrItems)
{
    auto config = two_asset_config();
    config.modbus.devices.push_back(modbus_device());
    const ItemTable table{config};
    EXPECT_EQ(table.find("unknown-plc", 0), nullptr);
    EXPECT_EQ(table.find("press-plc", 1), nullptr);
    EXPECT_EQ(table.source_index("unknown-plc"), std::nullopt);
}

TEST(ItemTable, SupportsModbusOnlyConfigurations)
{
    auto config = two_asset_config();
    config.opcua.reset();
    config.assets.clear();
    config.modbus.devices.push_back(modbus_device());
    auto second = modbus_device();
    second.source_id = "oven-plc";
    second.points[0].asset = "site01.line01.oven02";
    config.modbus.devices.push_back(second);
    const ItemTable table{config};
    ASSERT_EQ(table.sources().size(), 2U);
    EXPECT_EQ(table.sources()[0].source_id, "press-plc");
    EXPECT_EQ(table.sources()[1].source_id, "oven-plc");
    EXPECT_EQ(table.find(opc(), 0), nullptr);
    EXPECT_EQ(table.find("oven-plc", 0)->asset, "site01.line01.oven02");
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
    const auto point = make_point(*table.find(opc(), 0), change, kEdgeTime);
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
        const auto point = make_point(*table.find(opc(), 2), change, kEdgeTime);
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
    const auto point = make_point(*table.find(opc(), 1), change, kEdgeTime);
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
    EXPECT_FALSE(make_point(*table.find(opc(), 0), change, kEdgeTime).has_value());
}

TEST(ReceiptTime, IsCurrentSystemTime)
{
    const auto before = std::chrono::system_clock::now();
    const auto receipt = detail::receipt_time();
    const auto after = std::chrono::system_clock::now();
    EXPECT_GE(receipt, std::chrono::time_point_cast<std::chrono::nanoseconds>(before));
    EXPECT_LE(receipt, std::chrono::time_point_cast<std::chrono::nanoseconds>(after));
}

// ---- Startup validation -------------------------------------------------------------------

TEST(ValidateRuntimeConfig, AcceptsQos1)
{
    auto config = two_asset_config();
    config.mqtt.qos = config::MqttQos::AtLeastOnce;
    EXPECT_NO_THROW(validate_runtime_config(config));
}

TEST(ValidateRuntimeConfig, RejectsQos0And2Clearly)
{
    for (const auto qos : {config::MqttQos::AtMostOnce, config::MqttQos::ExactlyOnce}) {
        auto config = two_asset_config();
        config.mqtt.qos = qos;
        try {
            validate_runtime_config(config);
            ADD_FAILURE() << "expected StartupError";
        } catch (const StartupError& error) {
            EXPECT_THAT(error.what(), HasSubstr("mqtt.qos must be 1"));
            EXPECT_THAT(error.what(), HasSubstr("got " + std::to_string(static_cast<int>(qos))));
        }
    }
}

TEST(ValidateRuntimeConfig, AcceptsMixedAndModbusOnlyConfigurations)
{
    auto mixed = two_asset_config();
    mixed.modbus.devices.push_back(modbus_device());
    EXPECT_NO_THROW(validate_runtime_config(mixed));
    auto modbus_only = mixed;
    modbus_only.opcua.reset();
    modbus_only.assets.clear();
    EXPECT_NO_THROW(validate_runtime_config(modbus_only));
}

TEST(MakeGatewayRuntime, BuildsTheMixedExampleWithoutNetworkActivity)
{
    const auto config = config::load_config_file(IEG_MIXED_EXAMPLE_CONFIG_PATH);
    ASSERT_FALSE(config.modbus.devices.empty());
    const auto runtime = make_gateway_runtime(config);
    ASSERT_NE(runtime, nullptr);
    const auto items = runtime->item_diagnostics();
    EXPECT_EQ(items.size(), 2U + config.modbus.devices[0].points.size());
    EXPECT_EQ(items.back().protocol, "modbus_tcp");
}

TEST(MakeGatewayRuntime, BuildsAModbusOnlyRuntime)
{
    auto config = two_asset_config();
    config.opcua.reset();
    config.assets.clear();
    config.mqtt.client_id = "ieg-unit-test";
    config.modbus.devices.push_back(modbus_device());
    const auto runtime = make_gateway_runtime(config);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->item_diagnostics().at(0).source_id, "press-plc");
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
