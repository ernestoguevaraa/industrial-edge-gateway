#include "ieg/config/config_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
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

    GatewayConfig parse_root(const YAML::Node& root)
    {
        GatewayConfig config;
        if (root.IsNull()) {
            add_error(root.Mark(), "",
                      "configuration is empty; expected a mapping with keys opcua, mqtt, assets");
            return config;
        }
        if (!expect_map(root, "")) {
            return config;
        }
        check_keys(root, "", {"opcua", "mqtt", "runtime", "assets"});

        if (const auto opcua = field(root, "opcua", "", true)) {
            config.opcua = parse_opcua(*opcua);
        }
        if (const auto mqtt = field(root, "mqtt", "", true)) {
            config.mqtt = parse_mqtt(*mqtt);
        }
        if (const auto runtime = field(root, "runtime", "", false)) {
            config.runtime = parse_runtime(*runtime);
        }
        if (const auto assets = field(root, "assets", "", true)) {
            config.assets = parse_assets(*assets);
        }
        return config;
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
            if (host->value.empty()) {
                add_error(host->mark, "mqtt.host", "must not be empty");
            } else if (contains_whitespace(host->value)) {
                add_error(host->mark, "mqtt.host",
                          std::format("'{}' must not contain whitespace", host->value));
            }
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
            if (!is_valid_asset(id->value)) {
                add_error(
                    id->mark, id_path,
                    std::format("'{}' is invalid; expected at least 3 dot-separated lowercase "
                                "segments matching [a-z0-9_-]+ (e.g. site01.line01.press01)",
                                id->value));
            }
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
            const auto tag_path = child_path(path, "tag");
            if (!is_valid_tag(tag->value)) {
                add_error(tag->mark, tag_path,
                          std::format("'{}' is invalid; expected a lowercase identifier "
                                      "matching [a-z0-9_-]+",
                                      tag->value));
            }
            if (asset) {
                check_unique(
                    tag_first_seen_, std::pair{asset->value, tag->value}, tag_path, tag->mark,
                    std::format("duplicate tag '{}' for asset '{}'", tag->value, asset->value));
            }
            result.tag = tag->value;
        }

        if (const auto unit = read_string(node, "unit", path, false)) {
            result.unit = unit->value;
        }
        return result;
    }

    std::string source_name_;
    std::vector<std::string> errors_;
    std::map<std::string, FirstSeen> asset_first_seen_;
    std::map<std::string, FirstSeen> node_id_first_seen_;
    std::map<std::pair<std::string, std::string>, FirstSeen> tag_first_seen_;
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
