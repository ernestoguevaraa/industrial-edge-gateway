#pragma once

#include "ieg/domain/data_quality.hpp"
#include "ieg/domain/telemetry.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ieg::acquisition {

// A source ID uniquely identifies a configured protocol/device adapter.
// Item IDs are unique only within their source. The runtime uses (source_id, item_id).
using ItemId = std::uint32_t;

struct Item
{
    ItemId id{};
    // Adapter-native locator: OPC UA NodeId, Modbus address mapping key, etc.
    std::string locator;
};

struct Update
{
    std::string source_id;
    ItemId item_id{};
    std::optional<domain::TelemetryValue> value;
    domain::DataQuality quality{domain::DataQuality::Bad};
    // Absent when the source protocol does not provide a sample timestamp.
    std::optional<domain::Timestamp> source_timestamp;
    // Adapter-native diagnostic (e.g. OPC UA status code encoded as a string).
    // Never interpret this as a protocol-independent quality code.
    std::string diagnostic;
};

enum class ConnectionState : std::uint8_t
{
    Connecting,
    Connected,
    Disconnected
};

struct Status
{
    std::string source_id;
    ConnectionState state{ConnectionState::Disconnected};
    std::string detail;
};

struct Callbacks
{
    std::function<void(const Update&)> on_update;
    std::function<void(const Status&)> on_status;
};

// The callbacks execute on source-owned worker threads, must be non-blocking, and
// must not call start/stop. stop() waits until no callbacks can run afterwards.
class IAcquisitionSource
{
public:
    IAcquisitionSource() = default;
    virtual ~IAcquisitionSource() = default;
    IAcquisitionSource(const IAcquisitionSource&) = delete;
    IAcquisitionSource& operator=(const IAcquisitionSource&) = delete;
    IAcquisitionSource(IAcquisitionSource&&) = delete;
    IAcquisitionSource& operator=(IAcquisitionSource&&) = delete;

    // Must not block on network connectivity; adapters retry operational failures.
    // Call at most once per instance.
    virtual void start(std::vector<Item> items, Callbacks callbacks) = 0;
    // Idempotent. Must not be called from a source callback.
    virtual void stop() noexcept = 0;
};

} // namespace ieg::acquisition
