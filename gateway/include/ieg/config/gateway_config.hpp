#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ieg::config {

enum class MqttQos : std::uint8_t
{
    AtMostOnce = 0,
    AtLeastOnce = 1,
    ExactlyOnce = 2
};

struct OpcUaConfig
{
    std::string endpoint;
    std::chrono::milliseconds subscription_interval{};
};

struct MqttConfig
{
    std::string host;
    std::uint16_t port{};
    MqttQos qos{MqttQos::AtLeastOnce};
    std::string topic_prefix;
};

struct NodeConfig
{
    std::string node_id;
    std::string tag;
    std::string unit;
};

struct AssetConfig
{
    // Dot-separated hierarchy with at least three segments, e.g. "site01.line01.press01".
    std::string asset;
    std::vector<NodeConfig> nodes;
};

struct GatewayConfig
{
    OpcUaConfig opcua;
    MqttConfig mqtt;
    std::vector<AssetConfig> assets;
};

} // namespace ieg::config
