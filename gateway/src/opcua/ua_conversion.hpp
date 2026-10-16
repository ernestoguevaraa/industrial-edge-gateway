#pragma once

// Private to the open62541 adapter: may include open62541, must not be included by public
// headers.

#include "ieg/domain/telemetry.hpp"
#include "ieg/opcua/opcua_client.hpp"

#include <open62541.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace ieg::opcua::detail {

// Either a converted value or the local reason it could not be converted.
using VariantConversion = std::variant<domain::TelemetryValue, std::string>;

// Supports scalar Boolean, SByte..Int64, Byte..UInt64, Float, Double and String. Arrays,
// empty variants, other types and non-finite Float/Double values yield a reason string.
[[nodiscard]] VariantConversion convert_variant(const UA_Variant& variant);

// UA_DateTime (100 ns ticks since 1601-01-01) range that maps onto Timestamp (int64
// nanoseconds since 1970) without overflow. Bounds are checked before any arithmetic.
inline constexpr std::int64_t kMinConvertibleDateTime =
    (std::numeric_limits<std::int64_t>::min() / 100) + UA_DATETIME_UNIX_EPOCH;
inline constexpr std::int64_t kMaxConvertibleDateTime =
    (std::numeric_limits<std::int64_t>::max() / 100) + UA_DATETIME_UNIX_EPOCH;

// nullopt if the value is outside [kMinConvertibleDateTime, kMaxConvertibleDateTime].
[[nodiscard]] std::optional<domain::Timestamp> convert_date_time(UA_DateTime date_time) noexcept;

struct ConvertedDataChange
{
    DataChange change;
    // Non-empty when the adapter failed locally (value present but unusable, or missing
    // despite a non-Bad status). The raw status_code is never altered.
    std::string conversion_failure;
    // The server sent a source timestamp that Timestamp cannot represent.
    bool source_timestamp_out_of_range{false};
};

[[nodiscard]] ConvertedDataChange to_data_change(ItemId id, const UA_DataValue& data_value);

} // namespace ieg::opcua::detail
