#include "ieg/modbus/decoder.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ieg::modbus {
namespace {

using config::ModbusDataType;
using config::ModbusPointConfig;
using config::ModbusWordOrder;

static_assert(std::numeric_limits<float>::is_iec559, "float32 decoding requires IEEE 754 binary32");
static_assert(sizeof(float) == sizeof(std::uint32_t));

// Index of the point's first element in a block, or nullopt if any of its `width` elements lies
// outside the block. Arithmetic in std::size_t cannot overflow (width <= 2).
std::optional<std::size_t> index_in_block(std::uint16_t offset, std::uint16_t start,
                                          std::size_t size, std::uint32_t width) noexcept
{
    if (offset < start) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(offset) - start;
    if (index + width > size) {
        return std::nullopt;
    }
    return index;
}

std::uint32_t combine(std::uint16_t first, std::uint16_t second, ModbusWordOrder order) noexcept
{
    const auto high = order == ModbusWordOrder::HighWordFirst ? first : second;
    const auto low = order == ModbusWordOrder::HighWordFirst ? second : first;
    return (static_cast<std::uint32_t>(high) << 16U) | low;
}

// Validates the point options that do not depend on the data.
std::optional<DecodeError> option_problem(const ModbusPointConfig& point) noexcept
{
    const bool wide = config::modbus_width(point.type) == 2;
    if (wide != point.word_order.has_value()) {
        return DecodeError::InvalidWordOrder;
    }
    if (point.scale && (point.type == ModbusDataType::Bool || !std::isfinite(*point.scale) ||
                        *point.scale == 0.0)) {
        return DecodeError::InvalidScale;
    }
    return std::nullopt;
}

// Raw (unscaled) register value; float32 NaN/infinity is rejected here. `order` only matters for
// 32-bit types (`words` then holds two registers).
DecodeResult raw_value(ModbusDataType type, std::span<const std::uint16_t> words,
                       ModbusWordOrder order)
{
    switch (type) {
    case ModbusDataType::UInt16:
        return DecodeResult::success(std::uint64_t{words[0]});
    case ModbusDataType::Int16:
        return DecodeResult::success(std::int64_t{std::bit_cast<std::int16_t>(words[0])});
    case ModbusDataType::UInt32:
        return DecodeResult::success(std::uint64_t{combine(words[0], words[1], order)});
    case ModbusDataType::Int32:
        return DecodeResult::success(
            std::int64_t{std::bit_cast<std::int32_t>(combine(words[0], words[1], order))});
    case ModbusDataType::Float32: {
        const auto value = std::bit_cast<float>(combine(words[0], words[1], order));
        if (!std::isfinite(value)) {
            return DecodeResult::failure(DecodeError::NonFiniteValue);
        }
        return DecodeResult::success(static_cast<double>(value));
    }
    case ModbusDataType::Bool:
        break;
    }
    return DecodeResult::failure(DecodeError::TypeTableMismatch);
}

double as_double(const domain::TelemetryValue& value) noexcept
{
    if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value)) {
        return static_cast<double>(*unsigned_value); // exact: at most 32 significant bits
    }
    if (const auto* signed_value = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*signed_value); // exact: at most 32 significant bits
    }
    const auto* double_value = std::get_if<double>(&value);
    return double_value != nullptr ? *double_value : std::numeric_limits<double>::quiet_NaN();
}

DecodeResult apply_scale(DecodeResult raw, const std::optional<double>& scale)
{
    if (!raw.ok() || !scale) {
        return raw;
    }
    const double scaled = as_double(raw.value()) * *scale;
    if (!std::isfinite(scaled)) {
        return DecodeResult::failure(DecodeError::NonFiniteValue);
    }
    return DecodeResult::success(scaled);
}

} // namespace

std::string_view to_string(DecodeError error) noexcept
{
    switch (error) {
    case DecodeError::InsufficientData:
        return "insufficient_data";
    case DecodeError::TableMismatch:
        return "table_mismatch";
    case DecodeError::TypeTableMismatch:
        return "type_table_mismatch";
    case DecodeError::InvalidWordOrder:
        return "invalid_word_order";
    case DecodeError::InvalidScale:
        return "invalid_scale";
    case DecodeError::NonFiniteValue:
        return "non_finite_value";
    }
    return "unknown";
}

DecodeResult::DecodeResult(std::variant<domain::TelemetryValue, DecodeError> state)
    : state_(std::move(state))
{}

DecodeResult DecodeResult::success(domain::TelemetryValue value)
{
    return DecodeResult{std::variant<domain::TelemetryValue, DecodeError>{std::in_place_index<0>,
                                                                          std::move(value)}};
}

DecodeResult DecodeResult::failure(DecodeError error) noexcept
{
    return DecodeResult{
        std::variant<domain::TelemetryValue, DecodeError>{std::in_place_index<1>, error}};
}

bool DecodeResult::ok() const noexcept
{
    return state_.index() == 0;
}

const domain::TelemetryValue& DecodeResult::value() const
{
    if (const auto* value = std::get_if<0>(&state_)) {
        return *value;
    }
    throw std::logic_error("DecodeResult::value() called on a failed decode");
}

DecodeError DecodeResult::error() const
{
    if (const auto* error = std::get_if<1>(&state_)) {
        return *error;
    }
    throw std::logic_error("DecodeResult::error() called on a successful decode");
}

DecodeResult decode(const ModbusPointConfig& point, const RegisterBlock& block)
{
    if (block.table != point.table) {
        return DecodeResult::failure(DecodeError::TableMismatch);
    }
    if (config::is_bit_table(block.table) || point.type == ModbusDataType::Bool) {
        return DecodeResult::failure(DecodeError::TypeTableMismatch);
    }
    if (const auto problem = option_problem(point)) {
        return DecodeResult::failure(*problem);
    }
    const auto width = config::modbus_width(point.type);
    const auto index = index_in_block(point.offset, block.start, block.values.size(), width);
    if (!index) {
        return DecodeResult::failure(DecodeError::InsufficientData);
    }
    // option_problem() guarantees a word order for every 32-bit type; 16-bit types ignore it.
    const auto order = point.word_order.value_or(ModbusWordOrder::HighWordFirst);
    return apply_scale(raw_value(point.type, block.values.subspan(*index, width), order),
                       point.scale);
}

DecodeResult decode(const ModbusPointConfig& point, const BitBlock& block)
{
    if (block.table != point.table) {
        return DecodeResult::failure(DecodeError::TableMismatch);
    }
    if (!config::is_bit_table(block.table) || point.type != ModbusDataType::Bool) {
        return DecodeResult::failure(DecodeError::TypeTableMismatch);
    }
    if (const auto problem = option_problem(point)) {
        return DecodeResult::failure(*problem);
    }
    const auto index = index_in_block(point.offset, block.start, block.values.size(), 1);
    if (!index) {
        return DecodeResult::failure(DecodeError::InsufficientData);
    }
    return DecodeResult::success(block.values[*index] != 0U);
}

} // namespace ieg::modbus
