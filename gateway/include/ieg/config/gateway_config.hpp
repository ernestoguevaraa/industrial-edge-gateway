#pragma once

#include <chrono>
#include <cstddef>
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
    // Optional. Empty means the publisher's development fallback ("ieg-gateway-<hostname>"),
    // which is not a stable identity; configure one for production.
    std::string client_id;
};

// Optional "runtime" section; members keep these defaults when it is absent.
struct RuntimeConfig
{
    // Capacity of the in-memory telemetry queue, in telemetry points. How long it can bridge an
    // MQTT outage depends on the telemetry rate.
    std::size_t queue_capacity{10'000};
    // Upper bound for publishing the points still queued when the gateway shuts down.
    std::chrono::milliseconds shutdown_drain_timeout{3000};
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
    RuntimeConfig runtime;
    std::vector<AssetConfig> assets;
};

} // namespace ieg::config
