#pragma once

#include "ieg/domain/data_quality.hpp"
#include "ieg/domain/telemetry.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Protocol abstraction between the OPC UA adapter and the gateway core.
//
// This header is SDK-neutral: it must never include open62541 (or any other OPC UA SDK)
// headers or expose their types. Connection management, including automatic reconnect and
// restoration of monitored items, is an implementation responsibility of the adapter and
// deliberately not part of this interface.
namespace ieg::opcua {

// Raw OPC UA StatusCode (OPC 10000-4, 7.39), kept as a plain integer so no SDK type leaks.
using StatusCode = std::uint32_t;

// Caller-chosen identifier correlating updates with the requested item. The core uses it to
// look up asset/tag/unit; it must be unique within one start() call.
using ItemId = std::uint32_t;

struct MonitoredItem
{
    ItemId id{};
    // OPC UA string NodeId, e.g. "ns=2;s=Line01.Press01.Pressure".
    std::string node_id;

    friend bool operator==(const MonitoredItem&, const MonitoredItem&) = default;
};

struct DataChange
{
    ItemId id{};

    // Absent when the server delivered no value, when the item could not be monitored
    // (e.g. BadNodeIdUnknown), or when the adapter could not convert the value locally
    // (e.g. an unsupported type or an array). quality is Bad whenever value is absent.
    std::optional<domain::TelemetryValue> value;

    // Effective quality exposed by the adapter. Normally
    // quality_from_status_code(status_code), but NOT always: a local conversion failure
    // keeps the server's (possibly Good) status_code and reports quality Bad.
    domain::DataQuality quality{domain::DataQuality::Bad};

    // Raw StatusCode as received from the server. The adapter never manufactures a code
    // for a local failure; it logs the local reason instead.
    StatusCode status_code{};

    // Exactly as delivered by the server; absent if the server sent none. Never
    // substituted with the server timestamp or local time.
    std::optional<domain::Timestamp> source_timestamp;

    friend bool operator==(const DataChange&, const DataChange&) = default;
};

enum class ConnectionState : std::uint8_t
{
    // First connection attempt, or a new attempt after Disconnected.
    Connecting,
    // Session established and monitored items created (or restored after a reconnect).
    Connected,
    // Operational connectivity failure or loss while the adapter is active; the adapter
    // keeps retrying. Never reported for a deliberate stop().
    Disconnected
};

struct ConnectionStatus
{
    ConnectionState state{ConnectionState::Disconnected};
    // Human-readable reason, e.g. "BadConnectionClosed"; may be empty.
    std::string detail;

    friend bool operator==(const ConnectionStatus&, const ConnectionStatus&) = default;
};

// Callbacks run on the adapter's I/O thread, serialized (never concurrently with each other).
// They must be short and non-blocking (e.g. push to a bounded queue) and must not call
// start() or stop(). Arguments are valid only for the duration of the call.
struct ClientCallbacks
{
    std::function<void(const DataChange&)> on_data_change;
    std::function<void(const ConnectionStatus&)> on_status_change;
};

class IOpcUaClient
{
public:
    IOpcUaClient() = default;
    // Implementations must stop() in their destructor.
    virtual ~IOpcUaClient() = default;

    IOpcUaClient(const IOpcUaClient&) = delete;
    IOpcUaClient& operator=(const IOpcUaClient&) = delete;
    IOpcUaClient(IOpcUaClient&&) = delete;
    IOpcUaClient& operator=(IOpcUaClient&&) = delete;

    // Begins managed operation and returns without waiting for the network: connect, monitor
    // `items`, and after any connectivity loss reconnect with bounded backoff and restore the
    // monitored items. After a restore the server resends current values, so repeated values
    // are possible. Call at most once per instance.
    //
    // Throws std::invalid_argument for an invalid request (empty items, duplicate ids, empty or
    // unparsable node_id, missing callback) and std::logic_error if already started. Never
    // throws for connectivity problems; those are reported through on_status_change.
    virtual void start(std::vector<MonitoredItem> items, ClientCallbacks callbacks) = 0;

    // Closes the session and releases resources. Does not report Disconnected. When it
    // returns, no callback is running and none will run afterwards. Idempotent and safe if
    // start() was never called. Must not be called from inside a callback.
    virtual void stop() noexcept = 0;
};

// Maps the severity bits (31..30) of an OPC UA StatusCode: 00 Good, 01 Uncertain, 10/11 Bad.
[[nodiscard]] constexpr domain::DataQuality quality_from_status_code(StatusCode code) noexcept
{
    switch (code >> 30U) {
    case 0U:
        return domain::DataQuality::Good;
    case 1U:
        return domain::DataQuality::Uncertain;
    default:
        return domain::DataQuality::Bad;
    }
}

} // namespace ieg::opcua
