#include "ieg/config/config_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ieg::config {

namespace {

template <typename Range>
std::string join(const Range& items, std::string_view separator)
{
    std::string result;
    for (const auto& item : items) {
        if (!result.empty()) {
            result += separator;
        }
        result += item;
    }
    return result;
}

std::string child_path(std::string_view parent, std::string_view key)
{
    return parent.empty() ? std::string{key} : std::format("{}.{}", parent, key);
}

std::string_view kind_of(const YAML::Node& node)
{
    switch (node.Type()) {
    case YAML::NodeType::Null:
        return "null";
    case YAML::NodeType::Scalar:
        return "a scalar";
    case YAML::NodeType::Sequence:
        return "a sequence";
    case YAML::NodeType::Map:
        return "a mapping";
    case YAML::NodeType::Undefined:
        break;
    }
    return "nothing";
}

bool is_valid_endpoint(std::string_view endpoint)
{
    constexpr std::string_view scheme = "opc.tcp://";
    if (!endpoint.starts_with(scheme)) {
        return false;
    }
    const auto rest = endpoint.substr(scheme.size());
    return !rest.substr(0, rest.find('/')).empty();
}

bool contains_whitespace(std::string_view text)
{
    return std::ranges::any_of(text, [](unsigned char c) { return std::isspace(c) != 0; });
}

std::optional<std::string_view> topic_prefix_problem(std::string_view prefix)
{
    if (prefix.empty()) {
        return "must not be empty";
    }
    if (prefix.find_first_of("+#") != std::string_view::npos) {
        return "must not contain MQTT wildcards '+' or '#'";
    }
    if (prefix.find('\0') != std::string_view::npos) {
        return "must not contain NUL characters";
    }
    if (prefix.starts_with('$')) {
        return "must not start with '$' (reserved for broker topics)";
    }
    if (prefix.starts_with('/') || prefix.ends_with('/')) {
        return "must not start or end with '/'";
    }
    if (prefix.find("//") != std::string_view::npos) {
        return "must not contain empty topic levels ('//')";
    }
    return std::nullopt;
}

bool is_valid_asset(const std::string& asset)
{
    static const std::regex pattern{R"([a-z0-9_-]+(\.[a-z0-9_-]+){2,})"};
    return std::regex_match(asset, pattern);
}

bool is_valid_tag(const std::string& tag)
{
    static const std::regex pattern{R"([a-z0-9_-]+)"};
    return std::regex_match(tag, pattern);
}

bool is_valid_source_id(const std::string& id)
{
    static const std::regex pattern{R"([a-z0-9][a-z0-9_-]{0,63})"};
    return std::regex_match(id, pattern);
}

template <typename Enum>
struct EnumName
{
    std::string_view name;
    Enum value;
};

constexpr std::array<EnumName<ModbusTable>, 4> kModbusTables{{
    {"coils", ModbusTable::Coils},
    {"discrete_inputs", ModbusTable::DiscreteInputs},
    {"holding_registers", ModbusTable::HoldingRegisters},
    {"input_registers", ModbusTable::InputRegisters},
}};

constexpr std::array<EnumName<ModbusDataType>, 6> kModbusTypes{{
    {"bool", ModbusDataType::Bool},
    {"uint16", ModbusDataType::UInt16},
    {"int16", ModbusDataType::Int16},
    {"uint32", ModbusDataType::UInt32},
    {"int32", ModbusDataType::Int32},
    {"float32", ModbusDataType::Float32},
}};

constexpr std::array<EnumName<ModbusWordOrder>, 2> kModbusWordOrders{{
    {"high_word_first", ModbusWordOrder::HighWordFirst},
    {"low_word_first", ModbusWordOrder::LowWordFirst},
}};

template <typename Enum, std::size_t N>
std::string_view name_of(const std::array<EnumName<Enum>, N>& names, Enum value)
{
    const auto* const it = std::ranges::find(names, value, &EnumName<Enum>::value);
    return it == names.end() ? std::string_view{"?"} : it->name;
}

template <typename Enum, std::size_t N>
std::string names_of(const std::array<EnumName<Enum>, N>& names)
{
    std::vector<std::string_view> list;
    list.reserve(N);
    for (const auto& entry : names) {
        list.push_back(entry.name);
    }
    return join(list, ", ");
}

// Syntactic check only; the OPC UA adapter performs the definitive NodeId parsing.
bool is_valid_node_id(const std::string& node_id)
{
    static const std::regex pattern{R"((ns=[0-9]+;|nsu=[^;]+;)?(i=[0-9]+|[sgb]=.+))"};
    return std::regex_match(node_id, pattern);
}

struct ScalarField
{
    std::string value;
    YAML::Mark mark;
};

template <typename T>
struct Marked
{
    T value;
    YAML::Mark mark;
};

struct FirstSeen
{
    std::string path;
    YAML::Mark mark;
};

class Parser
{
public:
    explicit Parser(std::string_view source_name) : source_name_{source_name} {}

    GatewayConfig parse(std::string_view yaml)
    {
        YAML::Node root;
        try {
            root = YAML::Load(std::string{yaml});
        } catch (const YAML::Exception& error) {
            add_error(error.mark, "", std::format("YAML syntax error: {}", error.msg));
            throw ConfigError(std::move(errors_));
        }

        GatewayConfig config = parse_root(root);
        if (!errors_.empty()) {
            throw ConfigError(std::move(errors_));
        }
        return config;
    }

private:
    void add_error(const YAML::Mark& mark, std::string_view path, std::string_view message)
    {
        std::string location{source_name_};
        if (!mark.is_null()) {
            location += std::format(":{}:{}", mark.line + 1, mark.column + 1);
        }
        errors_.push_back(path.empty() ? std::format("{}: {}", location, message)
                                       : std::format("{}: {}: {}", location, path, message));
    }

    bool expect_map(const YAML::Node& node, std::string_view path)
    {
        if (node.IsMap()) {
            return true;
        }
        add_error(node.Mark(), path, std::format("expected a mapping, got {}", kind_of(node)));
        return false;
    }

    bool expect_non_empty_sequence(const YAML::Node& node, std::string_view path,
                                   std::string_view item_name)
    {
        if (!node.IsSequence()) {
            add_error(node.Mark(), path, std::format("expected a sequence, got {}", kind_of(node)));
            return false;
        }
        if (node.size() == 0) {
            add_error(node.Mark(), path, std::format("must contain at least one {}", item_name));
            return false;
        }
        return true;
    }

    void check_keys(const YAML::Node& map, std::string_view path,
                    std::initializer_list<std::string_view> allowed)
    {
        std::set<std::string> seen;
        for (const auto& entry : map) {
            const YAML::Node& key = entry.first;
            if (!key.IsScalar()) {
                add_error(key.Mark(), path, "keys must be scalars");
                continue;
            }
            const std::string& name = key.Scalar();
            if (!seen.insert(name).second) {
                add_error(key.Mark(), path, std::format("duplicate key '{}'", name));
            } else if (std::ranges::find(allowed, name) == allowed.end()) {
                add_error(key.Mark(), path,
                          std::format("unknown key '{}'; expected one of: {}", name,
                                      join(allowed, ", ")));
            }
        }
    }

    std::optional<YAML::Node> field(const YAML::Node& map, std::string_view key,
                                    std::string_view path, bool required)
    {
        YAML::Node node = map[std::string{key}];
        if (node.IsDefined()) {
            return node;
        }
        if (required) {
            add_error(map.Mark(), path, std::format("missing required key '{}'", key));
        }
        return std::nullopt;
    }

    std::optional<ScalarField> read_string(const YAML::Node& map, std::string_view key,
                                           std::string_view path, bool required = true)
    {
        const auto node = field(map, key, path, required);
        if (!node) {
            return std::nullopt;
        }
        if (!node->IsScalar()) {
            add_error(node->Mark(), child_path(path, key),
                      std::format("expected a string, got {}", kind_of(*node)));
            return std::nullopt;
        }
        return ScalarField{.value = node->Scalar(), .mark = node->Mark()};
    }

    std::optional<std::int64_t> read_integer(const YAML::Node& map, std::string_view key,
                                             std::string_view path, std::int64_t min,
                                             std::int64_t max, std::string_view expected,
                                             bool required = true)
    {
        const auto node = field(map, key, path, required);
        if (!node) {
            return std::nullopt;
        }
        const auto key_path = child_path(path, key);
        if (!node->IsScalar()) {
            add_error(node->Mark(), key_path,
                      std::format("expected {}, got {}", expected, kind_of(*node)));
            return std::nullopt;
        }
        const std::string& text = node->Scalar();
        // Quoted scalars carry the "!" tag; "1883" is a string, not a number.
        if (node->Tag() == "!") {
            add_error(node->Mark(), key_path,
                      std::format("expected {}, got quoted string \"{}\"", expected, text));
            return std::nullopt;
        }
        std::int64_t value{};
        const auto* const end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        if (ec != std::errc{} || ptr != end) {
            add_error(node->Mark(), key_path, std::format("expected {}, got '{}'", expected, text));
            return std::nullopt;
        }
        if (value < min || value > max) {
            add_error(node->Mark(), key_path,
                      std::format("invalid value {}; expected {}", value, expected));
            return std::nullopt;
        }
        return value;
    }

    template <typename Key>
    void check_unique(std::map<Key, FirstSeen>& seen, Key key, const std::string& path,
                      const YAML::Mark& mark, std::string_view problem)
    {
        const auto [it, inserted] = seen.try_emplace(std::move(key), FirstSeen{path, mark});
        if (inserted) {
            return;
        }
        const FirstSeen& first = it->second;
        const auto line =
            first.mark.is_null() ? std::string{} : std::format(", line {}", first.mark.line + 1);
        add_error(mark, path, std::format("{} (first at {}{})", problem, first.path, line));
    }

    // Like read_integer, but also returns the value's position for later cross-field errors.
    std::optional<Marked<std::int64_t>>
    read_marked_integer(const YAML::Node& map, std::string_view key, std::string_view path,
                        std::int64_t min, std::int64_t max, std::string_view expected,
                        bool required = true)
    {
        const auto value = read_integer(map, key, path, min, max, expected, required);
        if (!value) {
            return std::nullopt;
        }
        return Marked<std::int64_t>{.value = *value, .mark = map[std::string{key}].Mark()};
    }

    template <typename Enum, std::size_t N>
    std::optional<Marked<Enum>>
    read_enum(const YAML::Node& map, std::string_view key, std::string_view path,
              const std::array<EnumName<Enum>, N>& names, bool required = true)
    {
        const auto text = read_string(map, key, path, required);
        if (!text) {
            return std::nullopt;
        }
        const auto* const it = std::ranges::find(names, text->value, &EnumName<Enum>::name);
        if (it == names.end()) {
            add_error(text->mark, child_path(path, key),
                      std::format("invalid value '{}'; expected one of: {}", text->value,
                                  names_of(names)));
            return std::nullopt;
        }
        return Marked<Enum>{.value = it->value, .mark = text->mark};
    }

    // Finite, non-zero number (integer or decimal notation; quoted strings are rejected).
    std::optional<Marked<double>> read_scale(const YAML::Node& map, std::string_view key,
                                             std::string_view path)
    {
        const auto node = field(map, key, path, false);
        if (!node) {
            return std::nullopt;
        }
        const auto key_path = child_path(path, key);
        constexpr std::string_view expected = "a finite, non-zero number";
        if (!node->IsScalar() || node->Tag() == "!") {
            add_error(node->Mark(), key_path,
                      std::format("expected {}, got {}", expected,
                                  node->IsScalar() ? "a quoted string" : kind_of(*node)));
            return std::nullopt;
        }
        const std::string& text = node->Scalar();
        double value{};
        const auto* const end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(text.data(), end, value);
        if (ec != std::errc{} || ptr != end || !std::isfinite(value) || value == 0.0) {
            add_error(node->Mark(), key_path, std::format("expected {}, got '{}'", expected, text));
            return std::nullopt;
        }
        return Marked<double>{.value = value, .mark = node->Mark()};
    }

    void check_host(const ScalarField& host, const std::string& path)
    {
        if (host.value.empty()) {
            add_error(host.mark, path, "must not be empty");
        } else if (contains_whitespace(host.value)) {
            add_error(host.mark, path, std::format("'{}' must not contain whitespace", host.value));
        }
    }

    void check_asset(const ScalarField& asset, const std::string& path)
    {
        if (!is_valid_asset(asset.value)) {
            add_error(asset.mark, path,
                      std::format("'{}' is invalid; expected at least 3 dot-separated lowercase "
                                  "segments matching [a-z0-9_-]+ (e.g. site01.line01.press01)",
                                  asset.value));
        }
    }

    // Validates the tag and enforces global (asset, tag) uniqueness across every source.
    void check_tag(const ScalarField& tag, const std::string& path,
                   const std::optional<ScalarField>& asset)
    {
        if (!is_valid_tag(tag.value)) {
            add_error(tag.mark, path,
                      std::format("'{}' is invalid; expected a lowercase identifier "
                                  "matching [a-z0-9_-]+",
                                  tag.value));
        }
        if (asset) {
            check_unique(tag_first_seen_, std::pair{asset->value, tag.value}, path, tag.mark,
                         std::format("duplicate tag '{}' for asset '{}'", tag.value, asset->value));
        }
    }

    GatewayConfig parse_root(const YAML::Node& root)
    {
        GatewayConfig config;
        if (root.IsNull()) {
            add_error(root.Mark(), "",
                      "configuration is empty; expected a mapping with key mqtt and at least one "
                      "acquisition source (opcua with assets, and/or modbus)");
            return config;
        }
        if (!expect_map(root, "")) {
            return config;
        }
        check_keys(root, "", {"opcua", "mqtt", "runtime", "assets", "modbus"});

        const auto opcua = field(root, "opcua", "", false);
        if (opcua) {
            config.opcua = parse_opcua(*opcua);
        }
        if (const auto mqtt = field(root, "mqtt", "", true)) {
            config.mqtt = parse_mqtt(*mqtt);
        }
        if (const auto runtime = field(root, "runtime", "", false)) {
            config.runtime = parse_runtime(*runtime);
        }
        const auto assets = field(root, "assets", "", false);
        if (assets) {
            config.assets = parse_assets(*assets);
        }
        const auto modbus = field(root, "modbus", "", false);
        if (modbus) {
            config.modbus = parse_modbus(*modbus);
        }
        check_sources(root, opcua.has_value(), assets.has_value(), modbus.has_value());
        return config;
    }

    // OPC UA is configured by opcua + assets together; at least one source must exist.
    void check_sources(const YAML::Node& root, bool opcua, bool assets, bool modbus)
    {
        if (opcua && !assets) {
            add_error(root.Mark(), "",
                      "missing required key 'assets' (required when 'opcua' is configured)");
        } else if (assets && !opcua) {
            add_error(root.Mark(), "",
                      "missing required key 'opcua' (required when 'assets' is configured)");
        } else if (!opcua && !modbus) {
            add_error(root.Mark(), "",
                      "no acquisition source configured; expected 'opcua' with 'assets' "
                      "(OPC UA) and/or 'modbus' (Modbus TCP)");
        }
    }

    OpcUaConfig parse_opcua(const YAML::Node& node)
    {
        constexpr std::string_view path = "opcua";
        OpcUaConfig opcua;
        if (!expect_map(node, path)) {
            return opcua;
        }
        check_keys(node, path, {"endpoint", "subscription_interval_ms"});

        if (const auto endpoint = read_string(node, "endpoint", path)) {
            if (!is_valid_endpoint(endpoint->value)) {
                add_error(
                    endpoint->mark, "opcua.endpoint",
                    std::format("'{}' is invalid; expected opc.tcp://<host>[:<port>][/<path>]",
                                endpoint->value));
            }
            opcua.endpoint = endpoint->value;
        }
        if (const auto interval = read_integer(node, "subscription_interval_ms", path, 1, 3'600'000,
                                               "an integer between 1 and 3600000")) {
            opcua.subscription_interval = std::chrono::milliseconds{*interval};
        }
        return opcua;
    }

    MqttConfig parse_mqtt(const YAML::Node& node)
    {
        constexpr std::string_view path = "mqtt";
        MqttConfig mqtt;
        if (!expect_map(node, path)) {
            return mqtt;
        }
        check_keys(node, path, {"host", "port", "qos", "topic_prefix", "client_id"});

        if (const auto host = read_string(node, "host", path)) {
            check_host(*host, "mqtt.host");
            mqtt.host = host->value;
        }
        if (const auto port =
                read_integer(node, "port", path, 1, 65'535, "an integer between 1 and 65535")) {
            mqtt.port = static_cast<std::uint16_t>(*port);
        }
        if (const auto qos = read_integer(node, "qos", path, 0, 2, "0, 1 or 2")) {
            mqtt.qos = static_cast<MqttQos>(*qos);
        }
        if (const auto prefix = read_string(node, "topic_prefix", path)) {
            if (const auto problem = topic_prefix_problem(prefix->value)) {
                add_error(prefix->mark, "mqtt.topic_prefix",
                          std::format("'{}' is invalid: {}", prefix->value, *problem));
            }
            mqtt.topic_prefix = prefix->value;
        }
        if (const auto client_id = read_string(node, "client_id", path, false)) {
            if (client_id->value.empty()) {
                add_error(client_id->mark, "mqtt.client_id", "must not be empty when present");
            } else if (contains_whitespace(client_id->value)) {
                add_error(client_id->mark, "mqtt.client_id",
                          std::format("'{}' must not contain whitespace", client_id->value));
            }
            mqtt.client_id = client_id->value;
        }
        return mqtt;
    }

    RuntimeConfig parse_runtime(const YAML::Node& node)
    {
        constexpr std::string_view path = "runtime";
        RuntimeConfig runtime;
        if (!expect_map(node, path)) {
            return runtime;
        }
        check_keys(node, path, {"queue_capacity", "shutdown_drain_timeout_ms"});

        if (const auto capacity = read_integer(node, "queue_capacity", path, 1, 1'000'000,
                                               "an integer between 1 and 1000000", false)) {
            runtime.queue_capacity = static_cast<std::size_t>(*capacity);
        }
        if (const auto drain = read_integer(node, "shutdown_drain_timeout_ms", path, 100, 60'000,
                                            "an integer between 100 and 60000", false)) {
            runtime.shutdown_drain_timeout = std::chrono::milliseconds{*drain};
        }
        return runtime;
    }

    std::vector<AssetConfig> parse_assets(const YAML::Node& node)
    {
        std::vector<AssetConfig> assets;
        if (!expect_non_empty_sequence(node, "assets", "asset")) {
            return assets;
        }
        std::size_t index = 0;
        for (const auto& asset_node : node) {
            assets.push_back(parse_asset(asset_node, std::format("assets[{}]", index++)));
        }
        return assets;
    }

    AssetConfig parse_asset(const YAML::Node& node, const std::string& path)
    {
        AssetConfig asset;
        if (!expect_map(node, path)) {
            return asset;
        }
        check_keys(node, path, {"asset", "nodes"});

        const auto id = read_string(node, "asset", path);
        if (id) {
            const auto id_path = child_path(path, "asset");
            check_asset(*id, id_path);
            check_unique(
                asset_first_seen_, id->value, id_path, id->mark,
                std::format("asset '{}' is declared in more than one asset block", id->value));
            asset.asset = id->value;
        }

        if (const auto nodes = field(node, "nodes", path, true)) {
            const auto nodes_path = child_path(path, "nodes");
            if (expect_non_empty_sequence(*nodes, nodes_path, "node")) {
                std::size_t index = 0;
                for (const auto& node_entry : *nodes) {
                    asset.nodes.push_back(
                        parse_node(node_entry, std::format("{}[{}]", nodes_path, index++), id));
                }
            }
        }
        return asset;
    }

    NodeConfig parse_node(const YAML::Node& node, const std::string& path,
                          const std::optional<ScalarField>& asset)
    {
        NodeConfig result;
        if (!expect_map(node, path)) {
            return result;
        }
        check_keys(node, path, {"node_id", "tag", "unit"});

        if (const auto node_id = read_string(node, "node_id", path)) {
            const auto node_id_path = child_path(path, "node_id");
            if (!is_valid_node_id(node_id->value)) {
                add_error(node_id->mark, node_id_path,
                          std::format("'{}' is invalid; expected an OPC UA NodeId such as "
                                      "ns=2;s=Line01.Press01.Pressure",
                                      node_id->value));
            }
            check_unique(node_id_first_seen_, node_id->value, node_id_path, node_id->mark,
                         std::format("duplicate node_id '{}'", node_id->value));
            result.node_id = node_id->value;
        }

        if (const auto tag = read_string(node, "tag", path)) {
            check_tag(*tag, child_path(path, "tag"), asset);
            result.tag = tag->value;
        }

        if (const auto unit = read_string(node, "unit", path, false)) {
            result.unit = unit->value;
        }
        return result;
    }

    // ---- Modbus TCP ---------------------------------------------------------------------------

    ModbusConfig parse_modbus(const YAML::Node& node)
    {
        constexpr std::string_view path = "modbus";
        ModbusConfig modbus;
        if (!expect_map(node, path)) {
            return modbus;
        }
        check_keys(node, path, {"devices"});
        if (const auto devices = field(node, "devices", path, true)) {
            if (expect_non_empty_sequence(*devices, "modbus.devices", "device")) {
                std::size_t index = 0;
                for (const auto& device : *devices) {
                    modbus.devices.push_back(
                        parse_modbus_device(device, std::format("modbus.devices[{}]", index++)));
                }
            }
        }
        return modbus;
    }

    void parse_source_id(const YAML::Node& node, const std::string& path, std::string& out)
    {
        const auto id = read_string(node, "source_id", path);
        if (!id) {
            return;
        }
        const auto id_path = child_path(path, "source_id");
        if (!is_valid_source_id(id->value)) {
            add_error(id->mark, id_path,
                      std::format("'{}' is invalid; expected 1 to 64 characters matching "
                                  "[a-z0-9][a-z0-9_-]*",
                                  id->value));
        } else if (id->value == kOpcUaSourceId) {
            add_error(id->mark, id_path,
                      std::format("'{}' is reserved for the OPC UA source", id->value));
        }
        check_unique(source_first_seen_, id->value, id_path, id->mark,
                     std::format("duplicate source_id '{}'", id->value));
        out = id->value;
    }

    ModbusDeviceConfig parse_modbus_device(const YAML::Node& node, const std::string& path)
    {
        ModbusDeviceConfig device;
        if (!expect_map(node, path)) {
            return device;
        }
        check_keys(
            node, path,
            {"source_id", "host", "port", "unit_id", "poll_interval_ms", "timeout_ms", "points"});
        parse_source_id(node, path, device.source_id);
        if (const auto host = read_string(node, "host", path)) {
            check_host(*host, child_path(path, "host"));
            device.host = host->value;
        }
        if (const auto port = read_integer(node, "port", path, 1, 65'535,
                                           "an integer between 1 and 65535", false)) {
            device.port = static_cast<std::uint16_t>(*port);
        }
        if (const auto unit_id =
                read_integer(node, "unit_id", path, 1, 255, "an integer between 1 and 255")) {
            device.unit_id = static_cast<std::uint8_t>(*unit_id);
        }
        parse_modbus_timing(node, path, device);
        if (const auto points = field(node, "points", path, true)) {
            const auto points_path = child_path(path, "points");
            if (expect_non_empty_sequence(*points, points_path, "point")) {
                std::size_t index = 0;
                for (const auto& point : *points) {
                    device.points.push_back(
                        parse_modbus_point(point, std::format("{}[{}]", points_path, index++)));
                }
            }
        }
        return device;
    }

    void parse_modbus_timing(const YAML::Node& node, const std::string& path,
                             ModbusDeviceConfig& device)
    {
        if (const auto poll = read_integer(node, "poll_interval_ms", path, 100, 3'600'000,
                                           "an integer between 100 and 3600000", false)) {
            device.poll_interval = std::chrono::milliseconds{*poll};
        }
        const auto timeout = read_marked_integer(node, "timeout_ms", path, 10, 60'000,
                                                 "an integer between 10 and 60000", false);
        if (!timeout) {
            // Only an explicit timeout can conflict with the poll interval.
            device.timeout = std::min(device.timeout, device.poll_interval);
            return;
        }
        device.timeout = std::chrono::milliseconds{timeout->value};
        if (device.timeout > device.poll_interval) {
            add_error(timeout->mark, child_path(path, "timeout_ms"),
                      std::format("{} ms must not exceed poll_interval_ms ({} ms)",
                                  device.timeout.count(), device.poll_interval.count()));
        }
    }

    ModbusPointConfig parse_modbus_point(const YAML::Node& node, const std::string& path)
    {
        ModbusPointConfig point;
        if (!expect_map(node, path)) {
            return point;
        }
        check_keys(node, path,
                   {"asset", "tag", "unit", "table", "offset", "type", "word_order", "scale"});
        const auto asset = read_string(node, "asset", path);
        if (asset) {
            check_asset(*asset, child_path(path, "asset"));
            point.asset = asset->value;
        }
        if (const auto tag = read_string(node, "tag", path)) {
            check_tag(*tag, child_path(path, "tag"), asset);
            point.tag = tag->value;
        }
        if (const auto unit = read_string(node, "unit", path, false)) {
            point.unit = unit->value;
        }
        const auto table = read_enum(node, "table", path, kModbusTables);
        const auto offset = read_marked_integer(node, "offset", path, 0, 65'535,
                                                "a zero-based address between 0 and 65535");
        const auto type = read_enum(node, "type", path, kModbusTypes);
        const auto word_order = read_enum(node, "word_order", path, kModbusWordOrders, false);
        const auto scale = read_scale(node, "scale", path);

        point.table = table ? table->value : point.table;
        point.offset = offset ? static_cast<std::uint16_t>(offset->value) : point.offset;
        point.type = type ? type->value : point.type;
        point.word_order =
            word_order ? std::optional{word_order->value} : std::optional<ModbusWordOrder>{};
        point.scale = scale ? std::optional{scale->value} : std::optional<double>{};
        if (type) {
            check_type_options(node, path, *type, word_order, scale);
            if (table) {
                check_type_in_table(path, *table, *type);
            }
            if (offset) {
                check_register_range(path, *offset, type->value);
            }
        }
        return point;
    }

    void check_type_in_table(const std::string& path, const Marked<ModbusTable>& table,
                             const Marked<ModbusDataType>& type)
    {
        const bool bool_type = type.value == ModbusDataType::Bool;
        if (is_bit_table(table.value) && !bool_type) {
            add_error(type.mark, child_path(path, "type"),
                      std::format("'{}' is not valid for table '{}'; coils and discrete_inputs "
                                  "only support bool",
                                  name_of(kModbusTypes, type.value),
                                  name_of(kModbusTables, table.value)));
        } else if (!is_bit_table(table.value) && bool_type) {
            add_error(type.mark, child_path(path, "type"),
                      std::format("'bool' is not valid for table '{}'; use coils or "
                                  "discrete_inputs (bits inside registers are not supported)",
                                  name_of(kModbusTables, table.value)));
        }
    }

    void check_register_range(const std::string& path, const Marked<std::int64_t>& offset,
                              ModbusDataType type)
    {
        constexpr std::int64_t kLastAddress = 65'535;
        const auto last = offset.value + modbus_width(type) - 1;
        if (last > kLastAddress) {
            add_error(offset.mark, child_path(path, "offset"),
                      std::format("'{}' at offset {} needs addresses {}..{}, beyond the last "
                                  "address {}",
                                  name_of(kModbusTypes, type), offset.value, offset.value, last,
                                  kLastAddress));
        }
    }

    void check_type_options(const YAML::Node& node, const std::string& path,
                            const Marked<ModbusDataType>& type,
                            const std::optional<Marked<ModbusWordOrder>>& word_order,
                            const std::optional<Marked<double>>& scale)
    {
        const auto type_name = name_of(kModbusTypes, type.value);
        const bool wide = modbus_width(type.value) == 2;
        if (wide && !word_order && !node["word_order"].IsDefined()) {
            add_error(node.Mark(), path,
                      std::format("missing required key 'word_order' for 32-bit type '{}'; "
                                  "expected one of: {}",
                                  type_name, names_of(kModbusWordOrders)));
        } else if (!wide && word_order) {
            add_error(word_order->mark, child_path(path, "word_order"),
                      std::format("is only valid for 32-bit types (uint32, int32, float32), not "
                                  "'{}'",
                                  type_name));
        }
        if (type.value == ModbusDataType::Bool && scale) {
            add_error(scale->mark, child_path(path, "scale"), "is not valid for type 'bool'");
        }
    }

    std::string source_name_;
    std::vector<std::string> errors_;
    std::map<std::string, FirstSeen> asset_first_seen_;
    std::map<std::string, FirstSeen> node_id_first_seen_;
    std::map<std::pair<std::string, std::string>, FirstSeen> tag_first_seen_;
    std::map<std::string, FirstSeen> source_first_seen_;
};

} // namespace

ConfigError::ConfigError(std::vector<std::string> errors)
    : std::runtime_error{join(errors, "\n")},
      errors_{std::make_shared<const std::vector<std::string>>(std::move(errors))}
{}

GatewayConfig load_config_file(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        throw ConfigError({std::format(
            "{}: configuration file does not exist or is not a regular file", path.string())});
    }
    const std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw ConfigError(
            {std::format("{}: cannot read configuration file; check permissions", path.string())});
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return parse_config(contents.str(), path.string());
}

GatewayConfig parse_config(std::string_view yaml, std::string_view source_name)
{
    return Parser{source_name}.parse(yaml);
}

} // namespace ieg::config
