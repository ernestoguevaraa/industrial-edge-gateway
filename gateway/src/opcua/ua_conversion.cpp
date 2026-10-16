#include "opcua/ua_conversion.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace ieg::opcua::detail {
namespace {

std::string type_name(const UA_DataType* type)
{
    return type != nullptr && type->typeName != nullptr ? type->typeName : "unknown type";
}

template <typename T>
const T& scalar(const UA_Variant& variant)
{
    return *static_cast<const T*>(variant.data);
}

VariantConversion finite_double(double value, const char* type)
{
    if (!std::isfinite(value)) {
        return std::string{"non-finite "} + type + " value unsupported";
    }
    return domain::TelemetryValue{value};
}

} // namespace

VariantConversion convert_variant(const UA_Variant& variant)
{
    if (UA_Variant_isEmpty(&variant)) {
        return std::string{"empty variant (no value)"};
    }
    if (!UA_Variant_isScalar(&variant)) {
        return "array of " + type_name(variant.type) + " (length " +
               std::to_string(variant.arrayLength) + ") unsupported";
    }

    const UA_DataType* type = variant.type;
    if (type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
        return domain::TelemetryValue{scalar<UA_Boolean>(variant)};
    }
    if (type == &UA_TYPES[UA_TYPES_SBYTE]) {
        return domain::TelemetryValue{std::int64_t{scalar<UA_SByte>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_INT16]) {
        return domain::TelemetryValue{std::int64_t{scalar<UA_Int16>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_INT32]) {
        return domain::TelemetryValue{std::int64_t{scalar<UA_Int32>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_INT64]) {
        return domain::TelemetryValue{std::int64_t{scalar<UA_Int64>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_BYTE]) {
        return domain::TelemetryValue{std::uint64_t{scalar<UA_Byte>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_UINT16]) {
        return domain::TelemetryValue{std::uint64_t{scalar<UA_UInt16>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_UINT32]) {
        return domain::TelemetryValue{std::uint64_t{scalar<UA_UInt32>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_UINT64]) {
        return domain::TelemetryValue{std::uint64_t{scalar<UA_UInt64>(variant)}};
    }
    if (type == &UA_TYPES[UA_TYPES_FLOAT]) {
        return finite_double(static_cast<double>(scalar<UA_Float>(variant)), "Float");
    }
    if (type == &UA_TYPES[UA_TYPES_DOUBLE]) {
        return finite_double(scalar<UA_Double>(variant), "Double");
    }
    if (type == &UA_TYPES[UA_TYPES_STRING]) {
        const auto& text = scalar<UA_String>(variant);
        if (text.length == 0) {
            return domain::TelemetryValue{std::string{}};
        }
        // UA_String is length-delimited UTF-8 without a terminator.
        return domain::TelemetryValue{
            std::string(reinterpret_cast<const char*>(text.data), // NOLINT(*-reinterpret-cast)
                        text.length)};
    }
    return "type " + type_name(type) + " unsupported";
}

std::optional<domain::Timestamp> convert_date_time(UA_DateTime date_time) noexcept
{
    if (date_time < kMinConvertibleDateTime || date_time > kMaxConvertibleDateTime) {
        return std::nullopt;
    }
    // In range, so neither the subtraction nor the multiplication can overflow.
    const std::int64_t unix_ticks = date_time - UA_DATETIME_UNIX_EPOCH;
    return domain::Timestamp{std::chrono::nanoseconds{unix_ticks * 100}};
}

ConvertedDataChange to_data_change(ItemId id, const UA_DataValue& data_value)
{
    ConvertedDataChange result;
    DataChange& change = result.change;
    change.id = id;
    // An omitted StatusCode is Good by definition of the DataValue encoding.
    change.status_code = data_value.hasStatus ? data_value.status : UA_STATUSCODE_GOOD;
    change.quality = quality_from_status_code(change.status_code);

    const bool has_value = data_value.hasValue && !UA_Variant_isEmpty(&data_value.value);
    if (has_value) {
        auto converted = convert_variant(data_value.value);
        if (auto* value = std::get_if<domain::TelemetryValue>(&converted)) {
            change.value = std::move(*value);
        } else {
            result.conversion_failure = std::get<std::string>(std::move(converted));
        }
    } else if (change.quality != domain::DataQuality::Bad) {
        // A Bad status without a value is normal; anything else without a value is not usable.
        result.conversion_failure = "no value despite non-Bad status";
    }
    if (!change.value) {
        change.quality = domain::DataQuality::Bad;
    }

    // 0 is the OPC UA "no time" value; never substitute another timestamp.
    if (data_value.hasSourceTimestamp && data_value.sourceTimestamp != 0) {
        change.source_timestamp = convert_date_time(data_value.sourceTimestamp);
        result.source_timestamp_out_of_range = !change.source_timestamp.has_value();
    }
    return result;
}

} // namespace ieg::opcua::detail
