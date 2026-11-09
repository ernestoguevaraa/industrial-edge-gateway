// Multi-source configuration: optional OPC UA, Modbus TCP devices and points, cross-source rules.

#include "ieg/config/config_loader.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ieg::config {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

constexpr std::string_view kMqtt = "mqtt:\n"
                                   "  host: localhost\n"
                                   "  port: 1883\n"
                                   "  qos: 1\n"
                                   "  topic_prefix: industrial\n";

constexpr std::string_view kOpcUa = "opcua:\n"
                                    "  endpoint: \"opc.tcp://localhost:4840\"\n"
                                    "  subscription_interval_ms: 1000\n"
                                    "assets:\n"
                                    "  - asset: site01.line01.press01\n"
                                    "    nodes:\n"
                                    "      - node_id: \"ns=2;s=Press01.Pressure\"\n"
                                    "        tag: pressure\n";

// One Modbus point; `extra` holds additional "key: value" lines separated by '\n' (without
// indentation). The default is a YAML comment, i.e. no additional key.
// Prefixes every '\n'-separated line of `lines` with `indent`.
std::string indented(std::string_view lines, std::string_view indent)
{
    std::string text;
    std::size_t start = 0;
    while (start <= lines.size()) {
        const auto end = std::min(lines.find('\n', start), lines.size());
        text += std::format("{}{}\n", indent, lines.substr(start, end - start));
        start = end + 1;
    }
    return text;
}

struct Point
{
    std::string asset = "site01.line01.comp01";
    std::string tag = "pressure";
    std::string table = "holding_registers";
    std::string offset = "0";
    std::string type = "uint16";
    std::string extra = "# no extra keys";

    [[nodiscard]] std::string str() const
    {
        std::string text = std::format("        - asset: {}\n"
                                       "          tag: {}\n"
                                       "          table: {}\n"
                                       "          offset: {}\n"
                                       "          type: {}\n",
                                       asset, tag, table, offset, type);
        text += indented(extra, "          ");
        return text;
    }
};

struct Device
{
    std::string source_id = "press-plc";
    std::string host = "plc";
    std::string unit_id = "1";
    std::string extra = "# no extra keys";
    std::vector<Point> points{Point{}};

    [[nodiscard]] std::string str() const
    {
        std::string text = std::format("    - source_id: {}\n"
                                       "      host: {}\n"
                                       "      unit_id: {}\n",
                                       source_id, host, unit_id);
        text += indented(extra, "      ");
        text += "      points:\n";
        for (const auto& point : points) {
            text += point.str();
        }
        return text;
    }
};

std::string modbus_section(const std::vector<Device>& devices)
{
    std::string text = "modbus:\n  devices:\n";
    for (const auto& device : devices) {
        text += device.str();
    }
    return text;
}

std::string modbus_only(const std::vector<Device>& devices)
{
    return std::string{kMqtt} + modbus_section(devices);
}

std::string with_point(Point point)
{
    Device device;
    device.points = {std::move(point)};
    return modbus_only({device});
}

std::string with_device(Device device)
{
    return modbus_only({std::move(device)});
}

std::vector<std::string> errors_for(const std::string& yaml)
{
    try {
        (void)parse_config(yaml);
    } catch (const ConfigError& error) {
        return error.errors();
    }
    ADD_FAILURE() << "expected ConfigError for:\n" << yaml;
    return {};
}

// Exactly one error, mentioning both the path and the problem.
::testing::Matcher<std::vector<std::string>> single_error(std::string_view path,
                                                          std::string_view problem)
{
    return ElementsAre(AllOf(HasSubstr(std::string{path}), HasSubstr(std::string{problem})));
}

ModbusPointConfig only_point(const std::string& yaml)
{
    const auto config = parse_config(yaml);
    return config.modbus.devices.at(0).points.at(0);
}

// ---- Backward compatibility ------------------------------------------------------------------

TEST(MultiSourceConfig, Mvp1ExampleStillLoadsAsOpcUaOnly)
{
    const auto config = load_config_file(IEG_EXAMPLE_CONFIG_PATH);
    const OpcUaConfig expected_opcua{.endpoint = "opc.tcp://plant-simulator:4840",
                                     .subscription_interval = std::chrono::milliseconds{1000}};
    EXPECT_EQ(config.opcua, std::optional{expected_opcua});
    EXPECT_THAT(config.assets, SizeIs(2));
    EXPECT_THAT(config.modbus.devices, IsEmpty());
}

TEST(MultiSourceConfig, OpcUaOnlyConfigurationHasNoModbusDevices)
{
    const auto config = parse_config(std::string{kMqtt} + std::string{kOpcUa});
    EXPECT_TRUE(config.opcua.has_value());
    EXPECT_THAT(config.modbus.devices, IsEmpty());
}

// ---- Valid Modbus configurations --------------------------------------------------------------

TEST(MultiSourceConfig, LoadsTheMixedExampleFile)
{
    const auto config = load_config_file(IEG_MIXED_EXAMPLE_CONFIG_PATH);
    ASSERT_TRUE(config.opcua.has_value());
    EXPECT_THAT(config.assets, SizeIs(2)); // the seven MVP 1 OPC UA nodes
    ASSERT_THAT(config.modbus.devices, SizeIs(1));
    const auto& device = config.modbus.devices[0];
    EXPECT_EQ(device.source_id, "compressor-plc");
    EXPECT_EQ(device.host, "modbus-simulator");
    EXPECT_EQ(device.port, 5020); // modbus-simulator/README.md
    EXPECT_EQ(device.unit_id, 1);
    EXPECT_EQ(device.poll_interval, std::chrono::milliseconds{1000});
    EXPECT_EQ(device.timeout, std::chrono::milliseconds{500});
    ASSERT_THAT(device.points, SizeIs(11));
}

TEST(MultiSourceConfig, MixedExamplePointsCoverEveryTableAndWordOrder)
{
    const auto config = load_config_file(IEG_MIXED_EXAMPLE_CONFIG_PATH);
    const auto& points = config.modbus.devices.at(0).points;
    ASSERT_THAT(points, SizeIs(11));
    const ModbusPointConfig oil_temperature{.asset = "site01.line01.compressor01",
                                            .tag = "oil_temperature",
                                            .unit = "degC",
                                            .table = ModbusTable::InputRegisters,
                                            .offset = 2,
                                            .type = ModbusDataType::Float32,
                                            .word_order = ModbusWordOrder::LowWordFirst,
                                            .scale = std::nullopt};
    EXPECT_EQ(points[9], oil_temperature);
    EXPECT_EQ(points[0].table, ModbusTable::Coils);
    EXPECT_EQ(points[2].table, ModbusTable::DiscreteInputs);
    EXPECT_EQ(points[4].scale, std::optional<double>{0.1});
    EXPECT_EQ(points[5].type, ModbusDataType::Int16);
    EXPECT_EQ(points[6].word_order, std::optional{ModbusWordOrder::HighWordFirst});
    EXPECT_EQ(points[8].type, ModbusDataType::Int32);
    EXPECT_EQ(points[10].type, ModbusDataType::UInt16);
}

TEST(MultiSourceConfig, ModbusOnlyConfigurationNeedsNoOpcUaSection)
{
    const auto config = parse_config(modbus_only({Device{}}));
    EXPECT_FALSE(config.opcua.has_value());
    EXPECT_THAT(config.assets, IsEmpty());
    ASSERT_THAT(config.modbus.devices, SizeIs(1));
}

TEST(MultiSourceConfig, AppliesDeviceDefaults)
{
    const auto device = parse_config(modbus_only({Device{}})).modbus.devices.at(0);
    EXPECT_EQ(device.port, 502);
    EXPECT_EQ(device.poll_interval, std::chrono::milliseconds{1000});
    EXPECT_EQ(device.timeout, std::chrono::milliseconds{1000});
}

TEST(MultiSourceConfig, ParsesExplicitDeviceSettings)
{
    Device input;
    input.unit_id = "255";
    input.extra = "port: 1502\npoll_interval_ms: 250\ntimeout_ms: 250";
    const auto device = parse_config(with_device(input)).modbus.devices.at(0);
    EXPECT_EQ(device.unit_id, 255);
    EXPECT_EQ(device.port, 1502);
    EXPECT_EQ(device.poll_interval, std::chrono::milliseconds{250});
    EXPECT_EQ(device.timeout, std::chrono::milliseconds{250});
}

TEST(MultiSourceConfig, AcceptsEveryValidTableAndTypeCombination)
{
    const std::vector<std::pair<std::string, std::string>> combinations{
        {"coils", "bool"},
        {"discrete_inputs", "bool"},
        {"holding_registers", "uint16"},
        {"holding_registers", "int16"},
        {"input_registers", "uint16"},
        {"input_registers", "int16"},
    };
    for (const auto& [table, type] : combinations) {
        const auto point = only_point(with_point({.table = table, .type = type}));
        EXPECT_EQ(point.word_order, std::nullopt) << table << "/" << type;
    }
}

TEST(MultiSourceConfig, AcceptsThirtyTwoBitTypesWithExplicitWordOrder)
{
    for (const std::string type : {"uint32", "int32", "float32"}) {
        for (const std::string table : {"holding_registers", "input_registers"}) {
            const auto point = only_point(
                with_point({.table = table, .type = type, .extra = "word_order: low_word_first"}));
            EXPECT_EQ(point.word_order, std::optional{ModbusWordOrder::LowWordFirst});
        }
    }
}

TEST(MultiSourceConfig, AcceptsHighestAddressesForEachWidth)
{
    EXPECT_EQ(only_point(with_point({.offset = "65535"})).offset, 65535);
    EXPECT_EQ(only_point(
                  with_point(
                      {.offset = "65534", .type = "int32", .extra = "word_order: high_word_first"}))
                  .offset,
              65534);
    EXPECT_EQ(only_point(with_point({.table = "coils", .offset = "65535", .type = "bool"})).offset,
              65535);
}

TEST(MultiSourceConfig, ParsesFiniteNonZeroScales)
{
    const std::vector<std::pair<std::string, double>> scales{
        {"0.1", 0.1}, {"-2", -2.0}, {"1e-3", 0.001}, {"10", 10.0}, {"1", 1.0}};
    for (const auto& [text, expected] : scales) {
        const auto point = only_point(with_point({.extra = "scale: " + text}));
        EXPECT_EQ(point.scale, std::optional<double>{expected}) << text;
    }
}

TEST(MultiSourceConfig, ScaleIsAbsentUnlessConfigured)
{
    EXPECT_EQ(only_point(with_point({})).scale, std::nullopt);
}

TEST(MultiSourceConfig, ParsesUnitAndKeepsItEmptyByDefault)
{
    EXPECT_EQ(only_point(with_point({.extra = "unit: bar"})).unit, "bar");
    EXPECT_EQ(only_point(with_point({})).unit, "");
}

TEST(MultiSourceConfig, AllowsSameAssetAcrossSourcesWithDifferentTags)
{
    Device device;
    device.points = {Point{.asset = "site01.line01.press01", .tag = "oil_temperature"}};
    const auto config =
        parse_config(std::string{kMqtt} + std::string{kOpcUa} + modbus_section({device}));
    EXPECT_EQ(config.modbus.devices.at(0).points.at(0).asset, "site01.line01.press01");
}

TEST(MultiSourceConfig, AcceptsSeveralDevicesWithDistinctSourceIds)
{
    Device second;
    second.source_id = "oven-plc";
    second.points = {Point{.tag = "temperature"}};
    const auto config = parse_config(modbus_only({Device{}, second}));
    ASSERT_THAT(config.modbus.devices, SizeIs(2));
    EXPECT_EQ(config.modbus.devices[1].source_id, "oven-plc");
}

// ---- Sources ---------------------------------------------------------------------------------

TEST(MultiSourceConfig, RejectsConfigurationWithoutAnySource)
{
    EXPECT_THAT(errors_for(std::string{kMqtt}),
                ElementsAre(HasSubstr("no acquisition source configured")));
}

TEST(MultiSourceConfig, RejectsOpcUaWithoutAssetsAndAssetsWithoutOpcUa)
{
    const std::string opcua_only =
        std::string{kOpcUa}.substr(0, std::string{kOpcUa}.find("assets:"));
    EXPECT_THAT(errors_for(std::string{kMqtt} + opcua_only),
                ElementsAre(HasSubstr("missing required key 'assets' (required when 'opcua' is "
                                      "configured)")));
    const std::string assets_only = std::string{kOpcUa}.substr(std::string{kOpcUa}.find("assets:"));
    EXPECT_THAT(errors_for(std::string{kMqtt} + assets_only + modbus_section({Device{}})),
                ElementsAre(HasSubstr("missing required key 'opcua' (required when 'assets' is "
                                      "configured)")));
}

TEST(MultiSourceConfig, RejectsMalformedModbusSection)
{
    EXPECT_THAT(errors_for(std::string{kMqtt} + "modbus: []\n"),
                ElementsAre(HasSubstr("modbus: expected a mapping, got a sequence")));
    EXPECT_THAT(errors_for(std::string{kMqtt} + "modbus: {}\n"),
                ElementsAre(HasSubstr("modbus: missing required key 'devices'")));
    EXPECT_THAT(errors_for(std::string{kMqtt} + "modbus:\n  devices: []\n"),
                ElementsAre(HasSubstr("modbus.devices: must contain at least one device")));
    EXPECT_THAT(errors_for(std::string{kMqtt} + "modbus:\n  devices: []\n  retries: 3\n"),
                UnorderedElementsAre(HasSubstr("modbus: unknown key 'retries'"),
                                     HasSubstr("must contain at least one device")));
}

// ---- Source IDs ------------------------------------------------------------------------------

TEST(MultiSourceConfig, RejectsDuplicateSourceIds)
{
    Device second;
    second.points = {Point{.tag = "temperature"}};
    EXPECT_THAT(errors_for(modbus_only({Device{}, second})),
                single_error("modbus.devices[1].source_id",
                             "duplicate source_id 'press-plc' (first at "
                             "modbus.devices[0].source_id"));
}

TEST(MultiSourceConfig, ReservesTheOpcUaSourceId)
{
    Device device;
    device.source_id = "opcua-main";
    EXPECT_THAT(errors_for(with_device(device)),
                single_error("modbus.devices[0].source_id", "reserved for the OPC UA source"));
}

TEST(MultiSourceConfig, RejectsInvalidSourceIds)
{
    const std::vector<std::string> ids{"Press-PLC", R"("press plc")",    R"("")", "-press",
                                       "a/b",       std::string(65, 'a')};
    for (const auto& id : ids) {
        Device device;
        device.source_id = id;
        EXPECT_THAT(errors_for(with_device(device)),
                    single_error("modbus.devices[0].source_id", "expected 1 to 64 characters"))
            << id;
    }
}

// ---- Device fields ---------------------------------------------------------------------------

TEST(MultiSourceConfig, RejectsInvalidHost)
{
    for (const std::string host : {R"("")", R"("plc 1")"}) {
        Device device;
        device.host = host;
        EXPECT_THAT(errors_for(with_device(device)),
                    ElementsAre(HasSubstr("modbus.devices[0].host")))
            << host;
    }
}

TEST(MultiSourceConfig, RejectsPortAndUnitIdOutOfRange)
{
    for (const std::string port : {"0", "65536", R"("502")"}) {
        Device device;
        device.extra = "port: " + port;
        EXPECT_THAT(errors_for(with_device(device)),
                    ElementsAre(HasSubstr("modbus.devices[0].port")))
            << port;
    }
    for (const std::string unit_id : {"0", "256", "-1"}) {
        Device device;
        device.unit_id = unit_id;
        EXPECT_THAT(errors_for(with_device(device)),
                    single_error("modbus.devices[0].unit_id", "between 1 and 255"))
            << unit_id;
    }
}

TEST(MultiSourceConfig, RejectsIntervalsAndTimeoutsOutOfRange)
{
    const std::vector<std::pair<std::string, std::string>> cases{
        {"poll_interval_ms: 99", "poll_interval_ms"},
        {"poll_interval_ms: 3600001", "poll_interval_ms"},
        {"timeout_ms: 9", "timeout_ms"},
        {"timeout_ms: 60001", "timeout_ms"},
    };
    for (const auto& [line, key] : cases) {
        Device device;
        device.extra = line;
        EXPECT_THAT(errors_for(with_device(device)),
                    ElementsAre(HasSubstr("modbus.devices[0]." + key)))
            << line;
    }
}

TEST(MultiSourceConfig, RejectsTimeoutLongerThanPollInterval)
{
    Device device;
    device.extra = "poll_interval_ms: 500\ntimeout_ms: 501";
    EXPECT_THAT(errors_for(with_device(device)),
                single_error("modbus.devices[0].timeout_ms",
                             "501 ms must not exceed poll_interval_ms (500 ms)"));
}

TEST(MultiSourceConfig, DefaultTimeoutNeverExceedsAShortPollInterval)
{
    Device device;
    device.extra = "poll_interval_ms: 200";
    EXPECT_EQ(parse_config(with_device(device)).modbus.devices.at(0).timeout,
              std::chrono::milliseconds{200});
}

TEST(MultiSourceConfig, RejectsMissingDeviceKeysAndUnknownKeys)
{
    EXPECT_THAT(errors_for(std::string{kMqtt} + "modbus:\n  devices:\n    - port: 502\n"),
                UnorderedElementsAre(HasSubstr("missing required key 'source_id'"),
                                     HasSubstr("missing required key 'host'"),
                                     HasSubstr("missing required key 'unit_id'"),
                                     HasSubstr("missing required key 'points'")));
    Device device;
    device.extra = "slave_id: 1";
    EXPECT_THAT(errors_for(with_device(device)),
                single_error("modbus.devices[0]", "unknown key 'slave_id'"));
}

TEST(MultiSourceConfig, RejectsDeviceWithoutPoints)
{
    Device device;
    device.points.clear();
    EXPECT_THAT(errors_for(with_device(device)),
                ElementsAre(HasSubstr("modbus.devices[0].points: expected a sequence, got null")));
}

// ---- Points: identity --------------------------------------------------------------------------

TEST(MultiSourceConfig, RejectsDuplicateAssetTagWithinModbus)
{
    Device device;
    device.points = {Point{}, Point{.offset = "1"}};
    EXPECT_THAT(errors_for(with_device(device)),
                single_error("modbus.devices[0].points[1].tag",
                             "duplicate tag 'pressure' for asset 'site01.line01.comp01' (first at "
                             "modbus.devices[0].points[0].tag"));
}

TEST(MultiSourceConfig, RejectsDuplicateAssetTagAcrossDevices)
{
    Device second;
    second.source_id = "oven-plc";
    EXPECT_THAT(errors_for(modbus_only({Device{}, second})),
                single_error("modbus.devices[1].points[0].tag",
                             "(first at modbus.devices[0].points[0].tag"));
}

TEST(MultiSourceConfig, RejectsDuplicateAssetTagAcrossOpcUaAndModbus)
{
    Device device;
    device.points = {Point{.asset = "site01.line01.press01", .tag = "pressure"}};
    EXPECT_THAT(errors_for(std::string{kMqtt} + std::string{kOpcUa} + modbus_section({device})),
                single_error("modbus.devices[0].points[0].tag",
                             "duplicate tag 'pressure' for asset 'site01.line01.press01' (first "
                             "at assets[0].nodes[0].tag"));
}

TEST(MultiSourceConfig, RejectsInvalidPointAssetAndTag)
{
    EXPECT_THAT(errors_for(with_point({.asset = "site01.comp01"})),
                single_error("modbus.devices[0].points[0].asset",
                             "expected at least 3 dot-separated lowercase segments"));
    EXPECT_THAT(errors_for(with_point({.tag = "Pressure"})),
                single_error("modbus.devices[0].points[0].tag", "expected a lowercase identifier"));
}

TEST(MultiSourceConfig, RejectsMissingPointKeys)
{
    Device device;
    device.points.clear();
    const std::string yaml = with_device(device) + "        - unit: bar\n";
    EXPECT_THAT(errors_for(yaml), UnorderedElementsAre(HasSubstr("missing required key 'asset'"),
                                                       HasSubstr("missing required key 'tag'"),
                                                       HasSubstr("missing required key 'table'"),
                                                       HasSubstr("missing required key 'offset'"),
                                                       HasSubstr("missing required key 'type'")));
}

TEST(MultiSourceConfig, RejectsUnknownPointKeysSuchAsAddress)
{
    EXPECT_THAT(errors_for(with_point({.extra = "address: 40001"})),
                single_error("modbus.devices[0].points[0]", "unknown key 'address'"));
}

// ---- Points: addressing ----------------------------------------------------------------------

TEST(MultiSourceConfig, RejectsOffsetsOutsideTheZeroBasedRange)
{
    for (const std::string offset : {"-1", "65536", R"("0")", "1.5", "0x10"}) {
        EXPECT_THAT(errors_for(with_point({.offset = offset})),
                    single_error("modbus.devices[0].points[0].offset",
                                 "a zero-based address between 0 and 65535"))
            << offset;
    }
}

TEST(MultiSourceConfig, RejectsThirtyTwoBitValueBeyondTheLastRegister)
{
    EXPECT_THAT(
        errors_for(with_point(
            {.offset = "65535", .type = "float32", .extra = "word_order: high_word_first"})),
        single_error("modbus.devices[0].points[0].offset",
                     "'float32' at offset 65535 needs addresses 65535..65536, beyond the "
                     "last address 65535"));
}

// ---- Points: tables and types ----------------------------------------------------------------

TEST(MultiSourceConfig, RejectsUnknownTableTypeAndWordOrder)
{
    EXPECT_THAT(errors_for(with_point({.table = "holding"})),
                single_error("points[0].table",
                             "invalid value 'holding'; expected one of: coils, discrete_inputs, "
                             "holding_registers, input_registers"));
    EXPECT_THAT(errors_for(with_point({.type = "float64"})),
                single_error("points[0].type", "invalid value 'float64'; expected one of: bool, "
                                               "uint16, int16, uint32, int32, float32"));
    for (const std::string order : {"ABCD", "big_endian", "high_byte_first"}) {
        EXPECT_THAT(errors_for(with_point({.type = "uint32", .extra = "word_order: " + order})),
                    single_error("points[0].word_order",
                                 "expected one of: high_word_first, low_word_first"))
            << order;
    }
}

TEST(MultiSourceConfig, RejectsRegisterTypesOnBitTables)
{
    for (const std::string table : {"coils", "discrete_inputs"}) {
        EXPECT_THAT(errors_for(with_point({.table = table, .type = "uint16"})),
                    single_error("points[0].type", "only support bool"))
            << table;
    }
}

TEST(MultiSourceConfig, RejectsBoolOnRegisterTables)
{
    for (const std::string table : {"holding_registers", "input_registers"}) {
        EXPECT_THAT(errors_for(with_point({.table = table, .type = "bool"})),
                    single_error("points[0].type", "bits inside registers are not supported"))
            << table;
    }
}

TEST(MultiSourceConfig, RequiresWordOrderForEveryThirtyTwoBitType)
{
    for (const std::string type : {"uint32", "int32", "float32"}) {
        EXPECT_THAT(
            errors_for(with_point({.type = type})),
            single_error("modbus.devices[0].points[0]",
                         "missing required key 'word_order' for 32-bit type '" + type + "'"))
            << type;
    }
}

TEST(MultiSourceConfig, RejectsWordOrderOnSixteenBitAndBoolTypes)
{
    for (const std::string type : {"uint16", "int16"}) {
        EXPECT_THAT(errors_for(with_point({.type = type, .extra = "word_order: high_word_first"})),
                    single_error("points[0].word_order", "is only valid for 32-bit types"))
            << type;
    }
    EXPECT_THAT(errors_for(with_point(
                    {.table = "coils", .type = "bool", .extra = "word_order: high_word_first"})),
                single_error("points[0].word_order", "not 'bool'"));
}

// ---- Points: scale ---------------------------------------------------------------------------

TEST(MultiSourceConfig, RejectsNonFiniteZeroOrMalformedScales)
{
    for (const std::string scale : {"0", "0.0", "-0", ".inf", "-.inf", ".nan", "inf", "nan",
                                    "1e400", "abc", R"("0.1")", "[1]"}) {
        EXPECT_THAT(errors_for(with_point({.extra = "scale: " + scale})),
                    single_error("points[0].scale", "expected a finite, non-zero number"))
            << scale;
    }
}

TEST(MultiSourceConfig, RejectsScaleOnBool)
{
    EXPECT_THAT(errors_for(with_point({.table = "coils", .type = "bool", .extra = "scale: 2"})),
                single_error("points[0].scale", "is not valid for type 'bool'"));
}

// ---- Aggregation -----------------------------------------------------------------------------

TEST(MultiSourceConfig, ReportsEveryModbusProblemAtOnce)
{
    Device device;
    device.unit_id = "0";
    device.points = {Point{.table = "coils", .type = "uint16"},
                     Point{.tag = "flow", .type = "float32", .extra = "scale: 0"}};
    EXPECT_THAT(errors_for(with_device(device)),
                UnorderedElementsAre(HasSubstr("modbus.devices[0].unit_id"),
                                     HasSubstr("points[0].type"), HasSubstr("points[1].scale"),
                                     HasSubstr("missing required key 'word_order'")));
}

} // namespace
} // namespace ieg::config
