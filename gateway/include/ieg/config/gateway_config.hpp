#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

    friend bool operator==(const OpcUaConfig&, const OpcUaConfig&) = default;
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

// ---- Modbus TCP --------------------------------------------------------------------------------
//
// Data convention (fixed; see docs/MVP2_MODBUS_DESIGN.md):
//  - `offset` is the ZERO-BASED PDU address within its table (holding register 40001 is
//    table holding_registers, offset 0). A 32-bit value occupies registers offset and offset+1.
//  - Within one 16-bit register the byte order is always big-endian (high byte first), as the
//    Modbus Application Protocol specifies; it is not configurable.
//  - 32-bit types need an explicit word order: high_word_first means register `offset` holds
//    bits 31..16 ("ABCD"); low_word_first means it holds bits 15..0 ("CDAB"). Byte-swapped
//    layouts ("BADC", "DCBA") are not supported.
//  - int16/int32 are two's complement; float32 is IEEE 754 binary32.
//  - Coils and discrete inputs are bool only; registers are never bool.
//  - Without `scale`, integer types yield integers and float32 yields a double. With `scale`,
//    the value is raw * scale as a double.

enum class ModbusTable : std::uint8_t
{
    Coils,
    DiscreteInputs,
    HoldingRegisters,
    InputRegisters
};

enum class ModbusDataType : std::uint8_t
{
    Bool,
    UInt16,
    Int16,
    UInt32,
    Int32,
    Float32
};

enum class ModbusWordOrder : std::uint8_t
{
    HighWordFirst,
    LowWordFirst
};

// Number of 16-bit registers (or bits, for Bool) a value of `type` occupies.
[[nodiscard]] constexpr std::uint32_t modbus_width(ModbusDataType type) noexcept
{
    switch (type) {
    case ModbusDataType::UInt32:
    case ModbusDataType::Int32:
    case ModbusDataType::Float32:
        return 2;
    case ModbusDataType::Bool:
    case ModbusDataType::UInt16:
    case ModbusDataType::Int16:
        return 1;
    }
    return 1;
}

[[nodiscard]] constexpr bool is_bit_table(ModbusTable table) noexcept
{
    return table == ModbusTable::Coils || table == ModbusTable::DiscreteInputs;
}

struct ModbusPointConfig
{
    std::string asset;
    std::string tag;
    std::string unit;
    ModbusTable table{ModbusTable::HoldingRegisters};
    std::uint16_t offset{};
    ModbusDataType type{ModbusDataType::UInt16};
    // Present exactly for 32-bit types.
    std::optional<ModbusWordOrder> word_order;
    // Finite and non-zero when present; never present for Bool.
    std::optional<double> scale;

    friend bool operator==(const ModbusPointConfig&, const ModbusPointConfig&) = default;
};

struct ModbusDeviceConfig
{
    // Unique acquisition source ID, e.g. "press-plc"; kOpcUaSourceId is reserved.
    std::string source_id;
    std::string host;
    std::uint16_t port{502};
    // 1..255 (0 is the broadcast address, which never answers a read).
    std::uint8_t unit_id{};
    std::chrono::milliseconds poll_interval{1000};
    // Per request; never longer than poll_interval. Defaults to min(1000 ms, poll_interval).
    std::chrono::milliseconds timeout{1000};
    std::vector<ModbusPointConfig> points;
};

// Optional "modbus" section.
struct ModbusConfig
{
    std::vector<ModbusDeviceConfig> devices;
};

// Source ID of the single OPC UA source configured by the "opcua" section.
inline constexpr std::string_view kOpcUaSourceId = "opcua-main";

// A validated configuration has at least one acquisition source: OPC UA (opcua + assets, always
// together) and/or one or more Modbus TCP devices. (asset, tag) is unique across all sources.
struct GatewayConfig
{
    // Absent in Modbus-only deployments.
    std::optional<OpcUaConfig> opcua;
    MqttConfig mqtt;
    RuntimeConfig runtime;
    // OPC UA nodes grouped by asset; non-empty exactly when opcua is present.
    std::vector<AssetConfig> assets;
    ModbusConfig modbus;
};

} // namespace ieg::config
