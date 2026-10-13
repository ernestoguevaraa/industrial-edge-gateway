#include "ieg/config/config_loader.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ieg::config {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::SizeIs;

constexpr std::string_view kSingleAsset = R"(assets:
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=Line01.Press01.Pressure"
        tag: pressure
        unit: bar
)";

// Builds a valid configuration; tests override single values. Line numbers:
// 1 opcua, 2 endpoint, 3 subscription_interval_ms, 4 mqtt, 5 host, 6 port, 7 qos,
// 8 topic_prefix, 9 assets, 10 asset, 11 nodes, 12 node_id, 13 tag, 14 unit.
struct TestYaml
{
    std::string endpoint = R"("opc.tcp://localhost:4840")";
    std::string interval = "1000";
    std::string host = "localhost";
    std::string port = "1883";
    std::string qos = "1";
    std::string topic_prefix = "industrial";
    std::string assets{kSingleAsset};

    [[nodiscard]] std::string str() const
    {
        return std::format("opcua:\n"
                           "  endpoint: {}\n"
                           "  subscription_interval_ms: {}\n"
                           "mqtt:\n"
                           "  host: {}\n"
                           "  port: {}\n"
                           "  qos: {}\n"
                           "  topic_prefix: {}\n"
                           "{}",
                           endpoint, interval, host, port, qos, topic_prefix, assets);
    }
};

std::string assets_with_node(std::string_view asset, std::string_view node_id, std::string_view tag)
{
    return std::format("assets:\n"
                       "  - asset: {}\n"
                       "    nodes:\n"
                       "      - node_id: \"{}\"\n"
                       "        tag: {}\n",
                       asset, node_id, tag);
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

// --- valid configurations ---------------------------------------------------

TEST(ConfigLoader, LoadsRepositoryExampleFile)
{
    const auto config = load_config_file(IEG_EXAMPLE_CONFIG_PATH);

    EXPECT_EQ(config.opcua.endpoint, "opc.tcp://plant-simulator:4840");
    EXPECT_EQ(config.opcua.subscription_interval, std::chrono::milliseconds{1000});
    EXPECT_EQ(config.mqtt.host, "mosquitto");
    EXPECT_EQ(config.mqtt.port, 1883);
    EXPECT_EQ(config.mqtt.qos, MqttQos::AtLeastOnce);
    EXPECT_EQ(config.mqtt.topic_prefix, "industrial");

    ASSERT_THAT(config.assets, SizeIs(2));
    EXPECT_EQ(config.assets[0].asset, "site01.line01.press01");
    ASSERT_THAT(config.assets[0].nodes, SizeIs(4));
    EXPECT_EQ(config.assets[0].nodes[0].tag, "state");
    EXPECT_EQ(config.assets[0].nodes[0].unit, "");
    EXPECT_EQ(config.assets[1].asset, "site01.line01.oven01");
    ASSERT_THAT(config.assets[1].nodes, SizeIs(3));
    EXPECT_EQ(config.assets[1].nodes[1].node_id, "ns=2;s=Line01.Oven01.Temperature");
    EXPECT_EQ(config.assets[1].nodes[1].tag, "temperature");
    EXPECT_EQ(config.assets[1].nodes[1].unit, "degC");
}

TEST(ConfigLoader, ParsesMinimalConfiguration)
{
    const auto config = parse_config(TestYaml{}.str());

    EXPECT_EQ(config.opcua.endpoint, "opc.tcp://localhost:4840");
    EXPECT_EQ(config.mqtt.port, 1883);
    ASSERT_THAT(config.assets, SizeIs(1));
    ASSERT_THAT(config.assets[0].nodes, SizeIs(1));
    EXPECT_EQ(config.assets[0].nodes[0].node_id, "ns=2;s=Line01.Press01.Pressure");
    EXPECT_EQ(config.assets[0].nodes[0].unit, "bar");
}

TEST(ConfigLoader, UnitIsOptional)
{
    TestYaml yaml;
    yaml.assets = assets_with_node("site01.line01.press01", "ns=2;s=A", "state");
    EXPECT_EQ(parse_config(yaml.str()).assets[0].nodes[0].unit, "");
}

TEST(ConfigLoader, AcceptsAllMqttQosLevels)
{
    const std::vector<std::pair<std::string, MqttQos>> cases{
        {"0", MqttQos::AtMostOnce}, {"1", MqttQos::AtLeastOnce}, {"2", MqttQos::ExactlyOnce}};
    for (const auto& [text, expected] : cases) {
        TestYaml yaml;
        yaml.qos = text;
        EXPECT_EQ(parse_config(yaml.str()).mqtt.qos, expected) << "qos " << text;
    }
}

TEST(ConfigLoader, AcceptsDeeperAssetHierarchies)
{
    for (const std::string_view asset :
         {"site01.line01.press01", "site01.area01.line01.press01",
          "enterprise01.site01.area01.line01.press01", "site-01.line_01.press01"}) {
        TestYaml yaml;
        yaml.assets = assets_with_node(asset, "ns=2;s=A", "pressure");
        EXPECT_EQ(parse_config(yaml.str()).assets[0].asset, asset);
    }
}

TEST(ConfigLoader, AcceptsCommonNodeIdForms)
{
    for (const std::string_view node_id :
         {"i=85", "ns=2;i=1001", "ns=2;s=Line01.Press01.Pressure", "nsu=urn:plant;s=Press01",
          "ns=1;g=09087e75-8e5e-499b-954f-f2a9603db28a", "ns=1;b=M/RbKBsRVkePCePcx24oRA=="}) {
        TestYaml yaml;
        yaml.assets = assets_with_node("site01.line01.press01", node_id, "pressure");
        EXPECT_EQ(parse_config(yaml.str()).assets[0].nodes[0].node_id, node_id);
    }
}

TEST(ConfigLoader, AcceptsMultiLevelTopicPrefix)
{
    TestYaml yaml;
    yaml.topic_prefix = "plant/edge";
    EXPECT_EQ(parse_config(yaml.str()).mqtt.topic_prefix, "plant/edge");
}

// --- malformed input --------------------------------------------------------

TEST(ConfigLoader, RejectsMissingFile)
{
    try {
        (void)load_config_file("does/not/exist.yaml");
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_THAT(error.errors(),
                    ElementsAre("does/not/exist.yaml: configuration file does not exist or is "
                                "not a regular file"));
    }
}

TEST(ConfigLoader, ReportsSourceNameAndPositionOfYamlSyntaxErrors)
{
    try {
        (void)parse_config("opcua: [\n", "gateway.yaml");
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_THAT(error.errors(),
                    ElementsAre("gateway.yaml:2:1: YAML syntax error: end of sequence flow not "
                                "found"));
    }
}

TEST(ConfigLoader, RejectsEmptyDocument)
{
    EXPECT_THAT(errors_for(""), ElementsAre(HasSubstr("configuration is empty")));
}

TEST(ConfigLoader, RejectsNonMappingRoot)
{
    EXPECT_THAT(errors_for("- a\n- b\n"),
                ElementsAre("<memory>:1:1: expected a mapping, got a sequence"));
}

TEST(ConfigLoader, RejectsMissingSections)
{
    EXPECT_THAT(errors_for("{}"), ElementsAre(HasSubstr("missing required key 'opcua'"),
                                              HasSubstr("missing required key 'mqtt'"),
                                              HasSubstr("missing required key 'assets'")));
}

TEST(ConfigLoader, RejectsMissingRequiredKey)
{
    const std::string yaml = "opcua:\n  subscription_interval_ms: 1000\n" +
                             TestYaml{}.str().substr(TestYaml{}.str().find("mqtt:"));
    EXPECT_THAT(errors_for(yaml),
                ElementsAre("<memory>:2:3: opcua: missing required key 'endpoint'"));
}

TEST(ConfigLoader, RejectsUnknownKey)
{
    TestYaml yaml;
    yaml.topic_prefix = "industrial\n  qso: 1";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:9:3: mqtt: unknown key 'qso'; expected one of: host, port, "
                            "qos, topic_prefix"));
}

TEST(ConfigLoader, RejectsDuplicateYamlKey)
{
    TestYaml yaml;
    yaml.topic_prefix = "industrial\n  qos: 2";
    EXPECT_THAT(errors_for(yaml.str()), ElementsAre("<memory>:9:3: mqtt: duplicate key 'qos'"));
}

TEST(ConfigLoader, RejectsSectionOfWrongType)
{
    EXPECT_THAT(errors_for("opcua: 1\nmqtt: []\nassets: {}\n"),
                ElementsAre("<memory>:1:8: opcua: expected a mapping, got a scalar",
                            "<memory>:2:7: mqtt: expected a mapping, got a sequence",
                            "<memory>:3:9: assets: expected a sequence, got a mapping"));
}

// --- MQTT -------------------------------------------------------------------

TEST(ConfigLoader, RejectsInvalidQos)
{
    TestYaml yaml;
    yaml.qos = "3";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:7:8: mqtt.qos: invalid value 3; expected 0, 1 or 2"));

    yaml.qos = "-1";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:7:8: mqtt.qos: invalid value -1; expected 0, 1 or 2"));
}

TEST(ConfigLoader, RejectsNonIntegerQos)
{
    TestYaml yaml;
    for (const std::string_view qos : {"one", "1.0", "0x1", "+1", "[1]"}) {
        yaml.qos = qos;
        EXPECT_THAT(errors_for(yaml.str()),
                    ElementsAre(AllOf(HasSubstr("mqtt.qos: expected 0, 1 or 2, got"))))
            << qos;
    }
}

TEST(ConfigLoader, RejectsQuotedNumbers)
{
    TestYaml yaml;
    yaml.port = R"("1883")";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:6:9: mqtt.port: expected an integer between 1 and 65535, "
                            "got quoted string \"1883\""));
}

TEST(ConfigLoader, RejectsPortOutOfRange)
{
    TestYaml yaml;
    for (const std::string_view port : {"0", "65536", "-1"}) {
        yaml.port = port;
        EXPECT_THAT(
            errors_for(yaml.str()),
            ElementsAre(HasSubstr(std::format(
                "mqtt.port: invalid value {}; expected an integer between 1 and 65535", port))));
    }
}

TEST(ConfigLoader, RejectsInvalidHost)
{
    TestYaml yaml;
    yaml.host = R"("")";
    EXPECT_THAT(errors_for(yaml.str()), ElementsAre("<memory>:5:9: mqtt.host: must not be empty"));

    yaml.host = R"("mos quitto")";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre(HasSubstr("mqtt.host: 'mos quitto' must not contain whitespace")));
}

TEST(ConfigLoader, RejectsInvalidTopicPrefix)
{
    const std::vector<std::pair<std::string, std::string>> cases{
        {R"("")", "must not be empty"},
        {R"("industrial/#")", "must not contain MQTT wildcards"},
        {R"("a/+/b")", "must not contain MQTT wildcards"},
        {R"("a\0b")", "must not contain NUL characters"},
        {R"("$SYS")", "must not start with '$'"},
        {R"("/industrial")", "must not start or end with '/'"},
        {R"("industrial/")", "must not start or end with '/'"},
        {R"("a//b")", "must not contain empty topic levels"},
    };
    for (const auto& [prefix, reason] : cases) {
        TestYaml yaml;
        yaml.topic_prefix = prefix;
        EXPECT_THAT(
            errors_for(yaml.str()),
            ElementsAre(AllOf(HasSubstr("<memory>:8:17: mqtt.topic_prefix: "), HasSubstr(reason))))
            << prefix;
    }
}

// --- OPC UA -----------------------------------------------------------------

TEST(ConfigLoader, RejectsInvalidEndpoint)
{
    for (const std::string_view endpoint :
         {R"("http://plant:4840")", R"("opc.tcp://")", R"("opc.tcp:///path")", R"("")"}) {
        TestYaml yaml;
        yaml.endpoint = endpoint;
        EXPECT_THAT(errors_for(yaml.str()),
                    ElementsAre(AllOf(HasSubstr("<memory>:2:13: opcua.endpoint: "),
                                      HasSubstr("expected opc.tcp://<host>[:<port>][/<path>]"))))
            << endpoint;
    }
}

TEST(ConfigLoader, RejectsSubscriptionIntervalOutOfRange)
{
    TestYaml yaml;
    for (const std::string_view interval : {"0", "3600001"}) {
        yaml.interval = interval;
        EXPECT_THAT(
            errors_for(yaml.str()),
            ElementsAre(HasSubstr(std::format(
                "<memory>:3:29: opcua.subscription_interval_ms: invalid value {}", interval))));
    }
}

// --- assets -----------------------------------------------------------------

TEST(ConfigLoader, RejectsEmptyAssetAndNodeLists)
{
    TestYaml yaml;
    yaml.assets = "assets: []\n";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:9:9: assets: must contain at least one asset"));

    yaml.assets = "assets:\n  - asset: site01.line01.press01\n    nodes: []\n";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:11:12: assets[0].nodes: must contain at least one node"));
}

TEST(ConfigLoader, RejectsInvalidAssetIdentifiers)
{
    for (const std::string_view asset :
         {"site01.line01", "Site01.line01.press01", "site01..press01", "site01.line01.press01.",
          R"("site01.line01.press 01")", R"("site01/line01/press01")"}) {
        TestYaml yaml;
        yaml.assets = assets_with_node(asset, "ns=2;s=A", "pressure");
        EXPECT_THAT(errors_for(yaml.str()),
                    ElementsAre(AllOf(HasSubstr("<memory>:10:12: assets[0].asset: "),
                                      HasSubstr("expected at least 3 dot-separated lowercase "
                                                "segments"))))
            << asset;
    }
}

TEST(ConfigLoader, RejectsInvalidTags)
{
    for (const std::string_view tag : {"Pressure", R"("pres sure")", "a/b", R"("")"}) {
        TestYaml yaml;
        yaml.assets = assets_with_node("site01.line01.press01", "ns=2;s=A", tag);
        EXPECT_THAT(errors_for(yaml.str()),
                    ElementsAre(AllOf(HasSubstr("<memory>:13:14: assets[0].nodes[0].tag: "),
                                      HasSubstr("expected a lowercase identifier"))))
            << tag;
    }
}

TEST(ConfigLoader, RejectsInvalidNodeIds)
{
    for (const std::string_view node_id :
         {"Line01.Press01.Pressure", "ns=x;s=A", "ns=2;s=", "ns=2;i=abc", ""}) {
        TestYaml yaml;
        yaml.assets = assets_with_node("site01.line01.press01", node_id, "pressure");
        EXPECT_THAT(errors_for(yaml.str()),
                    ElementsAre(AllOf(HasSubstr("<memory>:12:18: assets[0].nodes[0].node_id: "),
                                      HasSubstr("expected an OPC UA NodeId"))))
            << node_id;
    }
}

TEST(ConfigLoader, RejectsMissingNodeFields)
{
    TestYaml yaml;
    yaml.assets = "assets:\n  - asset: site01.line01.press01\n    nodes:\n      - unit: bar\n";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:12:9: assets[0].nodes[0]: missing required key 'node_id'",
                            "<memory>:12:9: assets[0].nodes[0]: missing required key 'tag'"));
}

TEST(ConfigLoader, RejectsNullUnit)
{
    TestYaml yaml;
    yaml.assets =
        assets_with_node("site01.line01.press01", "ns=2;s=A", "pressure") + "        unit:\n";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre(HasSubstr("assets[0].nodes[0].unit: expected a string, got null")));
}

// --- duplicates -------------------------------------------------------------

TEST(ConfigLoader, RejectsDuplicateAssetTag)
{
    TestYaml yaml;
    yaml.assets = R"(assets:
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=A"
        tag: pressure
      - node_id: "ns=2;s=B"
        tag: pressure
)";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:15:14: assets[0].nodes[1].tag: duplicate tag 'pressure' for "
                            "asset 'site01.line01.press01' (first at assets[0].nodes[0].tag, line "
                            "13)"));
}

TEST(ConfigLoader, AllowsSameTagOnDifferentAssets)
{
    TestYaml yaml;
    yaml.assets = R"(assets:
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=A"
        tag: state
  - asset: site01.line01.oven01
    nodes:
      - node_id: "ns=2;s=B"
        tag: state
)";
    EXPECT_THAT(parse_config(yaml.str()).assets, SizeIs(2));
}

TEST(ConfigLoader, RejectsAssetDeclaredInMultipleBlocks)
{
    TestYaml yaml;
    yaml.assets = R"(assets:
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=A"
        tag: pressure
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=B"
        tag: state
)";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:14:12: assets[1].asset: asset 'site01.line01.press01' is "
                            "declared in more than one asset block (first at assets[0].asset, "
                            "line 10)"));
}

TEST(ConfigLoader, RejectsDuplicateNodeIdAcrossAssets)
{
    TestYaml yaml;
    yaml.assets = R"(assets:
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=Shared"
        tag: pressure
  - asset: site01.line01.oven01
    nodes:
      - node_id: "ns=2;s=Shared"
        tag: temperature
)";
    EXPECT_THAT(errors_for(yaml.str()),
                ElementsAre("<memory>:16:18: assets[1].nodes[0].node_id: duplicate node_id "
                            "'ns=2;s=Shared' (first at assets[0].nodes[0].node_id, line 12)"));
}

// --- aggregation ------------------------------------------------------------

TEST(ConfigLoader, ReportsAllErrorsAtOnce)
{
    TestYaml yaml;
    yaml.endpoint = R"("http://plant")";
    yaml.port = "0";
    yaml.qos = "3";
    yaml.topic_prefix = "industrial\n  qso: 1";

    try {
        (void)parse_config(yaml.str(), "gateway.yaml");
        FAIL() << "expected ConfigError";
    } catch (const ConfigError& error) {
        EXPECT_THAT(error.errors(),
                    ElementsAre(HasSubstr("gateway.yaml:2:13: opcua.endpoint"),
                                HasSubstr("gateway.yaml:9:3: mqtt: unknown key 'qso'"),
                                HasSubstr("gateway.yaml:6:9: mqtt.port"),
                                HasSubstr("gateway.yaml:7:8: mqtt.qos")));
        EXPECT_EQ(std::string{error.what()}, error.errors()[0] + "\n" + error.errors()[1] + "\n" +
                                                 error.errors()[2] + "\n" + error.errors()[3]);
    }
}

} // namespace
} // namespace ieg::config
