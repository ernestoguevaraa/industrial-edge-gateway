#pragma once

#include "ieg/domain/data_quality.hpp"
#include "ieg/domain/telemetry.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// SDK-neutral acquisition ingress shared by every protocol adapter. This header must never
// include a protocol SDK or expose its types.
namespace ieg::acquisition {

// A source ID uniquely identifies a configured protocol/device adapter.
// Item IDs are unique only within their source. The runtime uses (source_id, item_id).
using ItemId = std::uint32_t;

// Raw status in the source protocol's own code space (e.g. an OPC UA StatusCode or a Modbus
// exception code). Opaque to the runtime: it is recorded and logged for diagnostics but never
// interpreted. Update::quality is the only protocol-independent quality.
using NativeStatusCode = std::uint32_t;

struct Item
{
    ItemId id{};
    // Adapter-native locator: OPC UA NodeId, Modbus address mapping key, etc.
    std::string locator;

    friend bool operator==(const Item&, const Item&) = default;
};

struct Update
{
    std::string source_id;
    ItemId item_id{};
    // Absent when the source delivered no usable value; quality is then Bad.
    std::optional<domain::TelemetryValue> value;
    domain::DataQuality quality{domain::DataQuality::Bad};
    // Absent when the source protocol does not provide a sample timestamp. Never substituted.
    std::optional<domain::Timestamp> source_timestamp;
    // Raw protocol status exactly as received; absent if the protocol reports none.
    std::optional<NativeStatusCode> native_status;

    friend bool operator==(const Update&, const Update&) = default;
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

    friend bool operator==(const Status&, const Status&) = default;
};

struct Callbacks
{
    std::function<void(const Update&)> on_update;
    std::function<void(const Status&)> on_status;
};

// Lifecycle contract:
//  - Callbacks execute on source-owned worker threads, serialized per source. They must be
//    short and non-blocking and must not call start()/stop(). Arguments are valid only for the
//    duration of the call.
//  - start() is called at most once per instance (also after stop()).
//  - stop() is idempotent, safe before start() and after a start() that threw, and must not be
//    called from a callback. When it returns no callback is running and none will run later.
class IAcquisitionSource
{
public:
    IAcquisitionSource() = default;
    // Implementations must stop() in their destructor.
    virtual ~IAcquisitionSource() = default;
    IAcquisitionSource(const IAcquisitionSource&) = delete;
    IAcquisitionSource& operator=(const IAcquisitionSource&) = delete;
    IAcquisitionSource(IAcquisitionSource&&) = delete;
    IAcquisitionSource& operator=(IAcquisitionSource&&) = delete;

    // Stable, non-empty identifier; every Update and Status carries it as source_id.
    [[nodiscard]] virtual const std::string& id() const noexcept = 0;

    // Begins acquisition of `items` and returns without waiting for the network; adapters retry
    // operational failures and report them through on_status. Throws std::invalid_argument for
    // an invalid request (no items, duplicate ids, empty locator, missing callback), which does
    // not consume the single start, and std::logic_error if already started or stopped.
    virtual void start(std::vector<Item> items, Callbacks callbacks) = 0;

    virtual void stop() noexcept = 0;
};

} // namespace ieg::acquisition
