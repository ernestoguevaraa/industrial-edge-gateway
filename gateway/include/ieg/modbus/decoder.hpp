#pragma once

#include "ieg/config/gateway_config.hpp"
#include "ieg/domain/telemetry.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

// Transport-independent decoding of Modbus data into TelemetryValue.
//
// Pure functions: no I/O, no global state, no external libraries. The input is what a Modbus read
// returns after the transport has parsed the PDU:
//  - registers as 16-bit values (each assembled from its big-endian byte pair, see
//    register_from_bytes()), or
//  - bits as one element per coil/discrete input (0 = false, any other value = true).
//
// Conventions (docs/MVP2_MODBUS_DESIGN.md, "Data convention"):
//  - uint16/int16 use one register; uint32/int32/float32 use registers offset and offset+1,
//    combined by word order: high_word_first -> (r[offset] << 16) | r[offset+1] ("ABCD"),
//    low_word_first -> (r[offset+1] << 16) | r[offset] ("CDAB").
//  - int16/int32 are two's complement; float32 is IEEE 754 binary32 (std::bit_cast).
//  - Without scale: bool -> bool, uint16/uint32 -> std::uint64_t, int16/int32 -> std::int64_t,
//    float32 -> double (exact widening). With scale: double(raw) * scale.
//  - Values are combined with shifts and masks only, so the result never depends on the CPU's
//    byte order.
namespace ieg::modbus {

// Why a point could not be decoded. Every failure is reported; nothing is clamped or guessed.
enum class DecodeError : std::uint8_t
{
    // The block does not contain every address the point needs (offset before the block start,
    // or offset + width beyond its end).
    InsufficientData,
    // The block was read from a different table than the point is configured for.
    TableMismatch,
    // bool requested from a register table, or a register type from a bit table.
    TypeTableMismatch,
    // word_order missing for a 32-bit type, or present for a 16-bit or bool type.
    InvalidWordOrder,
    // scale is zero or not finite, or present for bool.
    InvalidScale,
    // float32 NaN or infinity, or a scaled result that is not finite (overflow).
    NonFiniteValue,
};

[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

// Either a decoded value or the reason it could not be decoded.
class DecodeResult
{
public:
    [[nodiscard]] static DecodeResult success(domain::TelemetryValue value);
    [[nodiscard]] static DecodeResult failure(DecodeError error) noexcept;

    [[nodiscard]] bool ok() const noexcept;
    // Precondition ok(); throws std::logic_error otherwise.
    [[nodiscard]] const domain::TelemetryValue& value() const;
    // Precondition !ok(); throws std::logic_error otherwise.
    [[nodiscard]] DecodeError error() const;

    friend bool operator==(const DecodeResult&, const DecodeResult&) = default;

private:
    explicit DecodeResult(std::variant<domain::TelemetryValue, DecodeError> state);

    std::variant<domain::TelemetryValue, DecodeError> state_;
};

// Contiguous registers read from `table`, where values[0] is the register at address `start`.
struct RegisterBlock
{
    config::ModbusTable table{config::ModbusTable::HoldingRegisters};
    std::uint16_t start{};
    std::span<const std::uint16_t> values;
};

// Contiguous bits read from `table`, where values[0] is the bit at address `start`.
struct BitBlock
{
    config::ModbusTable table{config::ModbusTable::Coils};
    std::uint16_t start{};
    std::span<const std::uint8_t> values;
};

// The register value of a big-endian byte pair as it appears in a Modbus PDU.
[[nodiscard]] constexpr std::uint16_t register_from_bytes(std::uint8_t high,
                                                          std::uint8_t low) noexcept
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(high) << 8U) | low);
}

// Decodes `point` from a register block (holding or input registers).
[[nodiscard]] DecodeResult decode(const config::ModbusPointConfig& point,
                                  const RegisterBlock& block);

// Decodes `point` from a bit block (coils or discrete inputs).
[[nodiscard]] DecodeResult decode(const config::ModbusPointConfig& point, const BitBlock& block);

} // namespace ieg::modbus
