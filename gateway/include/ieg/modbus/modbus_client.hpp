#pragma once

#include "ieg/config/gateway_config.hpp"
#include "ieg/modbus/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// SDK-neutral Modbus client abstraction. This header must never include libmodbus (or any other
// Modbus library) or expose its types, so a fake can replace the real client in poller tests.
namespace ieg::modbus {

// Protocol limits per request (Modbus Application Protocol V1.1b3, function codes 01-04).
inline constexpr std::size_t kMaxReadBits = 2000;     // FC01 read coils, FC02 discrete inputs
inline constexpr std::size_t kMaxReadRegisters = 125; // FC03 holding, FC04 input registers
inline constexpr std::size_t kAddressSpace = 65'536;  // addresses 0..65535 per table

enum class ModbusErrorKind : std::uint8_t
{
    // Rejected before any I/O: wrong table for the call, quantity 0 or above the protocol limit,
    // or addresses beyond 65535.
    InvalidRequest,
    // read called while no connection is established (never connected, closed, or closed by
    // the client after an earlier failure).
    NotConnected,
    // connect() failed: refused, unreachable, name resolution, or connect timeout.
    ConnectionFailed,
    // The established connection failed during a request (reset, closed by the peer, ...).
    ConnectionLost,
    // No (complete) response within the configured timeout.
    Timeout,
    // The server answered with a Modbus exception response (e.g. 0x02 Illegal Data Address).
    ModbusException,
    // A response arrived but is not a valid answer to the request (transaction ID, length,
    // unknown exception code, ...).
    ProtocolError,
};

[[nodiscard]] std::string_view to_string(ModbusErrorKind kind) noexcept;

struct ModbusError
{
    ModbusErrorKind kind{ModbusErrorKind::ProtocolError};
    // The Modbus exception code from the server; present only for ModbusException.
    std::optional<std::uint8_t> exception_code;
    // The operating-system errno; present only when the failure came from the OS (connection
    // failures, timeouts). Never a Modbus exception code or a library-specific code.
    std::optional<int> system_errno;
    // Human-readable detail for logs.
    std::string detail;

    friend bool operator==(const ModbusError&, const ModbusError&) = default;
};

// Either a value or a ModbusError.
template <typename T>
class ModbusResult
{
public:
    [[nodiscard]] static ModbusResult success(T value)
    {
        return ModbusResult{std::variant<T, ModbusError>{std::in_place_index<0>, std::move(value)}};
    }
    [[nodiscard]] static ModbusResult failure(ModbusError error)
    {
        return ModbusResult{std::variant<T, ModbusError>{std::in_place_index<1>, std::move(error)}};
    }

    [[nodiscard]] bool ok() const noexcept { return state_.index() == 0; }

    // Precondition ok(); throws std::logic_error otherwise.
    [[nodiscard]] const T& value() const
    {
        if (const auto* value = std::get_if<0>(&state_)) {
            return *value;
        }
        throw std::logic_error("ModbusResult::value() called on a failed result");
    }

    // Precondition !ok(); throws std::logic_error otherwise.
    [[nodiscard]] const ModbusError& error() const
    {
        if (const auto* error = std::get_if<1>(&state_)) {
            return *error;
        }
        throw std::logic_error("ModbusResult::error() called on a successful result");
    }

private:
    explicit ModbusResult(std::variant<T, ModbusError> state) : state_(std::move(state)) {}

    std::variant<T, ModbusError> state_;
};

using ConnectResult = ModbusResult<std::monostate>;
using RegisterReadResult = ModbusResult<RegisterBlock>;
using BitReadResult = ModbusResult<BitBlock>;

// Checks a read request against the protocol limits and the table kind, without any I/O.
// `bits` selects coils/discrete inputs (true) or holding/input registers (false).
[[nodiscard]] std::optional<ModbusError>
validate_read(config::ModbusTable table, std::uint16_t start, std::size_t quantity, bool bits);

// Blocking Modbus client for one device (one unit ID on one TCP endpoint).
//
// Threading: not thread-safe. Exactly one owner thread calls every member function, including
// close() and the destructor. The client creates no threads.
//
// Blocking and timeouts: connect() and each read block the owner thread, bounded by the
// configured timeout (see the implementation for the exact bound). There is no cancellation:
// a stop request is honoured once the current call returns. Requests are never retried.
//
// Connection state: a read never connects implicitly. After ConnectionLost, Timeout or
// ProtocolError the client closes the connection itself (a late or partial response would
// otherwise desynchronise the stream) and connected() becomes false; the owner must call
// connect() explicitly. ModbusException responses are valid answers and keep the connection.
class IModbusClient
{
public:
    IModbusClient() = default;
    // Implementations close the connection in their destructor.
    virtual ~IModbusClient() = default;
    IModbusClient(const IModbusClient&) = delete;
    IModbusClient& operator=(const IModbusClient&) = delete;
    IModbusClient(IModbusClient&&) = delete;
    IModbusClient& operator=(IModbusClient&&) = delete;

    // (Re)establishes the connection; an existing connection is closed first (explicit
    // reconnect).
    [[nodiscard]] virtual ConnectResult connect() = 0;

    [[nodiscard]] virtual bool connected() const noexcept = 0;

    // Reads out.size() registers of `table` (holding or input registers) starting at `start`
    // into `out`. On success the block refers to `out` and carries `table` and `start`.
    [[nodiscard]] virtual RegisterReadResult read_registers(config::ModbusTable table,
                                                            std::uint16_t start,
                                                            std::span<std::uint16_t> out) = 0;

    // Reads out.size() bits of `table` (coils or discrete inputs) into `out`, one element per
    // bit (0 or 1). On success the block refers to `out` and carries `table` and `start`.
    [[nodiscard]] virtual BitReadResult read_bits(config::ModbusTable table, std::uint16_t start,
                                                  std::span<std::uint8_t> out) = 0;

    // Closes the connection. Idempotent; safe before connect().
    virtual void close() noexcept = 0;
};

} // namespace ieg::modbus
