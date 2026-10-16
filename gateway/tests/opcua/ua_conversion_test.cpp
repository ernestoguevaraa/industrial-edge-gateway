#include "opcua/ua_conversion.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace ieg::opcua::detail {
namespace {

using domain::DataQuality;
using domain::TelemetryValue;
using domain::Timestamp;

constexpr UA_StatusCode kUncertainSensorNotAccurate = 0x40930000U;
constexpr UA_StatusCode kBadSensorFailure = 0x808C0000U;

// Owns a UA_Variant built through the open62541 API.
class Variant
{
public:
    Variant() { UA_Variant_init(&variant_); }
    ~Variant() { UA_Variant_clear(&variant_); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    Variant(Variant&&) = delete;
    Variant& operator=(Variant&&) = delete;

    template <typename T>
    Variant& scalar(T value, const UA_DataType& type)
    {
        UA_Variant_clear(&variant_);
        EXPECT_EQ(UA_Variant_setScalarCopy(&variant_, &value, &type), UA_STATUSCODE_GOOD);
        return *this;
    }

    template <typename T>
    Variant& array(const T* values, std::size_t size, const UA_DataType& type)
    {
        UA_Variant_clear(&variant_);
        EXPECT_EQ(UA_Variant_setArrayCopy(&variant_, values, size, &type), UA_STATUSCODE_GOOD);
        return *this;
    }

    [[nodiscard]] const UA_Variant& get() const { return variant_; }
    UA_Variant& get() { return variant_; }

private:
    UA_Variant variant_{};
};

TelemetryValue value_of(const VariantConversion& conversion)
{
    EXPECT_TRUE(std::holds_alternative<TelemetryValue>(conversion))
        << (std::holds_alternative<std::string>(conversion) ? std::get<std::string>(conversion)
                                                            : "");
    return std::holds_alternative<TelemetryValue>(conversion) ? std::get<TelemetryValue>(conversion)
                                                              : TelemetryValue{};
}

std::string failure_of(const VariantConversion& conversion)
{
    EXPECT_TRUE(std::holds_alternative<std::string>(conversion));
    return std::holds_alternative<std::string>(conversion) ? std::get<std::string>(conversion) : "";
}

// ---- Scalar conversions ----------------------------------------------------------------

TEST(ConvertVariant, Boolean)
{
    Variant v;
    EXPECT_EQ(
        value_of(convert_variant(v.scalar<UA_Boolean>(true, UA_TYPES[UA_TYPES_BOOLEAN]).get())),
        TelemetryValue{true});
}

TEST(ConvertVariant, SignedIntegersWidenToInt64)
{
    Variant v;
    EXPECT_EQ(value_of(convert_variant(v.scalar<UA_SByte>(-128, UA_TYPES[UA_TYPES_SBYTE]).get())),
              TelemetryValue{std::int64_t{-128}});
    EXPECT_EQ(value_of(convert_variant(v.scalar<UA_Int16>(-32768, UA_TYPES[UA_TYPES_INT16]).get())),
              TelemetryValue{std::int64_t{-32768}});
    EXPECT_EQ(value_of(convert_variant(
                  v.scalar<UA_Int32>(std::numeric_limits<UA_Int32>::min(), UA_TYPES[UA_TYPES_INT32])
                      .get())),
              TelemetryValue{std::int64_t{std::numeric_limits<std::int32_t>::min()}});
    EXPECT_EQ(value_of(convert_variant(
                  v.scalar<UA_Int64>(std::numeric_limits<UA_Int64>::min(), UA_TYPES[UA_TYPES_INT64])
                      .get())),
              TelemetryValue{std::numeric_limits<std::int64_t>::min()});
}

TEST(ConvertVariant, UnsignedIntegersWidenToUInt64)
{
    Variant v;
    EXPECT_EQ(value_of(convert_variant(v.scalar<UA_Byte>(255, UA_TYPES[UA_TYPES_BYTE]).get())),
              TelemetryValue{std::uint64_t{255}});
    EXPECT_EQ(
        value_of(convert_variant(v.scalar<UA_UInt16>(65535, UA_TYPES[UA_TYPES_UINT16]).get())),
        TelemetryValue{std::uint64_t{65535}});
    EXPECT_EQ(value_of(convert_variant(
                  v.scalar<UA_UInt32>(4294967295U, UA_TYPES[UA_TYPES_UINT32]).get())),
              TelemetryValue{std::uint64_t{4294967295U}});
    EXPECT_EQ(value_of(convert_variant(v.scalar<UA_UInt64>(std::numeric_limits<UA_UInt64>::max(),
                                                           UA_TYPES[UA_TYPES_UINT64])
                                           .get())),
              TelemetryValue{std::numeric_limits<std::uint64_t>::max()});
}

TEST(ConvertVariant, FloatWidensToDouble)
{
    Variant v;
    EXPECT_EQ(value_of(convert_variant(v.scalar<UA_Float>(0.1F, UA_TYPES[UA_TYPES_FLOAT]).get())),
              TelemetryValue{static_cast<double>(0.1F)});
    EXPECT_EQ(
        value_of(convert_variant(v.scalar<UA_Double>(842.4, UA_TYPES[UA_TYPES_DOUBLE]).get())),
        TelemetryValue{842.4});
}

TEST(ConvertVariant, NonFiniteFloatingPointIsRejected)
{
    Variant v;
    constexpr double inf = std::numeric_limits<double>::infinity();
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(
        failure_of(convert_variant(v.scalar<UA_Double>(nan, UA_TYPES[UA_TYPES_DOUBLE]).get())),
        "non-finite Double value unsupported");
    EXPECT_EQ(
        failure_of(convert_variant(v.scalar<UA_Double>(inf, UA_TYPES[UA_TYPES_DOUBLE]).get())),
        "non-finite Double value unsupported");
    EXPECT_EQ(
        failure_of(convert_variant(v.scalar<UA_Double>(-inf, UA_TYPES[UA_TYPES_DOUBLE]).get())),
        "non-finite Double value unsupported");
    EXPECT_EQ(failure_of(convert_variant(v.scalar<UA_Float>(std::numeric_limits<float>::infinity(),
                                                            UA_TYPES[UA_TYPES_FLOAT])
                                             .get())),
              "non-finite Float value unsupported");
    EXPECT_EQ(failure_of(convert_variant(v.scalar<UA_Float>(std::numeric_limits<float>::quiet_NaN(),
                                                            UA_TYPES[UA_TYPES_FLOAT])
                                             .get())),
              "non-finite Float value unsupported");
}

TEST(ConvertVariant, Strings)
{
    Variant v;
    std::string running{"RUNNING"};
    EXPECT_EQ(value_of(convert_variant(
                  v.scalar<UA_String>(UA_STRING(running.data()), UA_TYPES[UA_TYPES_STRING]).get())),
              TelemetryValue{std::string{"RUNNING"}});
    EXPECT_EQ(value_of(convert_variant(
                  v.scalar<UA_String>(UA_STRING_NULL, UA_TYPES[UA_TYPES_STRING]).get())),
              TelemetryValue{std::string{}});
}

// ---- Unsupported -----------------------------------------------------------------------

TEST(ConvertVariant, ArraysAreUnsupported)
{
    Variant v;
    const std::array<UA_Double, 3> values{1.0, 2.0, 3.0};
    EXPECT_EQ(failure_of(convert_variant(
                  v.array(values.data(), values.size(), UA_TYPES[UA_TYPES_DOUBLE]).get())),
              "array of Double (length 3) unsupported");
    const UA_Int32 one = 7;
    EXPECT_EQ(failure_of(convert_variant(v.array(&one, 1, UA_TYPES[UA_TYPES_INT32]).get())),
              "array of Int32 (length 1) unsupported");
}

TEST(ConvertVariant, EmptyArrayIsUnsupported)
{
    Variant v;
    UA_Variant_setArray(&v.get(), UA_EMPTY_ARRAY_SENTINEL, 0, &UA_TYPES[UA_TYPES_DOUBLE]);
    EXPECT_EQ(failure_of(convert_variant(v.get())), "array of Double (length 0) unsupported");
    UA_Variant_init(&v.get()); // the sentinel is not owned
}

TEST(ConvertVariant, EmptyVariantAndOtherTypesAreUnsupported)
{
    Variant v;
    EXPECT_EQ(failure_of(convert_variant(v.get())), "empty variant (no value)");
    EXPECT_EQ(
        failure_of(convert_variant(v.scalar<UA_DateTime>(0, UA_TYPES[UA_TYPES_DATETIME]).get())),
        "type DateTime unsupported");
    EXPECT_EQ(
        failure_of(convert_variant(
            v.scalar<UA_StatusCode>(UA_STATUSCODE_GOOD, UA_TYPES[UA_TYPES_STATUSCODE]).get())),
        "type StatusCode unsupported");
}

// ---- DateTime --------------------------------------------------------------------------

TEST(ConvertDateTime, PreservesFull100NanosecondResolution)
{
    using namespace std::chrono;
    // 2026-10-15T10:00:00.1234567Z
    const auto expected = Timestamp{sys_days{year{2026} / 10 / 15} + 10h} + nanoseconds{123456700};
    const UA_DateTime ticks = UA_DATETIME_UNIX_EPOCH + (expected.time_since_epoch().count() / 100);
    EXPECT_EQ(convert_date_time(ticks), expected);
    EXPECT_EQ(convert_date_time(UA_DATETIME_UNIX_EPOCH), Timestamp{});
}

TEST(ConvertDateTime, AcceptsExactRepresentableBounds)
{
    using std::chrono::nanoseconds;
    EXPECT_EQ(convert_date_time(kMaxConvertibleDateTime),
              Timestamp{nanoseconds{(std::numeric_limits<std::int64_t>::max() / 100) * 100}});
    EXPECT_EQ(convert_date_time(kMinConvertibleDateTime),
              Timestamp{nanoseconds{(std::numeric_limits<std::int64_t>::min() / 100) * 100}});
}

TEST(ConvertDateTime, RejectsValuesJustOutsideBounds)
{
    EXPECT_FALSE(convert_date_time(kMaxConvertibleDateTime + 1).has_value());
    EXPECT_FALSE(convert_date_time(kMinConvertibleDateTime - 1).has_value());
}

TEST(ConvertDateTime, RejectsExtremeValuesWithoutOverflow)
{
    EXPECT_FALSE(convert_date_time(std::numeric_limits<UA_DateTime>::max()).has_value());
    EXPECT_FALSE(convert_date_time(std::numeric_limits<UA_DateTime>::min()).has_value());
}

// ---- DataValue -> DataChange -----------------------------------------------------------

class DataValue
{
public:
    DataValue() { UA_DataValue_init(&value_); }
    ~DataValue() { UA_DataValue_clear(&value_); }
    DataValue(const DataValue&) = delete;
    DataValue& operator=(const DataValue&) = delete;
    DataValue(DataValue&&) = delete;
    DataValue& operator=(DataValue&&) = delete;

    DataValue& double_value(double value)
    {
        UA_Variant_setScalarCopy(&value_.value, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        value_.hasValue = true;
        return *this;
    }
    DataValue& status(UA_StatusCode code)
    {
        value_.status = code;
        value_.hasStatus = true;
        return *this;
    }
    DataValue& source_timestamp(UA_DateTime time)
    {
        value_.sourceTimestamp = time;
        value_.hasSourceTimestamp = true;
        return *this;
    }
    UA_DataValue& raw() { return value_; }
    [[nodiscard]] const UA_DataValue& get() const { return value_; }

private:
    UA_DataValue value_{};
};

constexpr UA_DateTime kSomeTime = UA_DATETIME_UNIX_EPOCH + 17'600'000'000'000'000LL;

TEST(ToDataChange, GoodValueWithSourceTimestamp)
{
    DataValue dv;
    dv.double_value(180.25).status(UA_STATUSCODE_GOOD).source_timestamp(kSomeTime);
    const auto result = to_data_change(7, dv.get());
    EXPECT_EQ(result.change.id, 7U);
    EXPECT_EQ(result.change.value, std::optional<TelemetryValue>{180.25});
    EXPECT_EQ(result.change.quality, DataQuality::Good);
    EXPECT_EQ(result.change.status_code, UA_STATUSCODE_GOOD);
    EXPECT_EQ(result.change.source_timestamp, convert_date_time(kSomeTime));
    EXPECT_TRUE(result.conversion_failure.empty());
}

TEST(ToDataChange, MissingStatusMeansGood)
{
    DataValue dv;
    dv.double_value(1.0);
    const auto result = to_data_change(1, dv.get());
    EXPECT_EQ(result.change.status_code, UA_STATUSCODE_GOOD);
    EXPECT_EQ(result.change.quality, DataQuality::Good);
}

TEST(ToDataChange, UncertainKeepsValue)
{
    DataValue dv;
    dv.double_value(851.2).status(kUncertainSensorNotAccurate);
    const auto result = to_data_change(1, dv.get());
    EXPECT_EQ(result.change.quality, DataQuality::Uncertain);
    EXPECT_EQ(result.change.value, std::optional<TelemetryValue>{851.2});
    EXPECT_EQ(result.change.status_code, kUncertainSensorNotAccurate);
}

TEST(ToDataChange, BadStatusPreservesConvertibleValue)
{
    DataValue dv;
    dv.double_value(171.4).status(kBadSensorFailure);
    const auto result = to_data_change(1, dv.get());
    EXPECT_EQ(result.change.quality, DataQuality::Bad);
    EXPECT_EQ(result.change.value, std::optional<TelemetryValue>{171.4});
    EXPECT_EQ(result.change.status_code, kBadSensorFailure);
    EXPECT_TRUE(result.conversion_failure.empty());
}

TEST(ToDataChange, BadStatusWithoutValueIsNotALocalFailure)
{
    DataValue dv;
    dv.status(kBadSensorFailure);
    const auto result = to_data_change(1, dv.get());
    EXPECT_FALSE(result.change.value.has_value());
    EXPECT_EQ(result.change.quality, DataQuality::Bad);
    EXPECT_TRUE(result.conversion_failure.empty());
}

TEST(ToDataChange, LocalConversionFailureKeepsRawGoodStatus)
{
    DataValue dv;
    const std::array<UA_Double, 2> values{1.0, 2.0};
    UA_Variant_setArrayCopy(&dv.raw().value, values.data(), values.size(),
                            &UA_TYPES[UA_TYPES_DOUBLE]);
    dv.raw().hasValue = true;
    dv.status(UA_STATUSCODE_GOOD).source_timestamp(kSomeTime);
    const auto result = to_data_change(3, dv.get());
    EXPECT_FALSE(result.change.value.has_value());
    EXPECT_EQ(result.change.quality, DataQuality::Bad);
    EXPECT_EQ(result.change.status_code, UA_STATUSCODE_GOOD);
    EXPECT_EQ(result.conversion_failure, "array of Double (length 2) unsupported");
    EXPECT_TRUE(result.change.source_timestamp.has_value());
}

TEST(ToDataChange, NonFiniteWithGoodStatusIsLocalFailure)
{
    DataValue dv;
    dv.double_value(std::numeric_limits<double>::quiet_NaN()).status(UA_STATUSCODE_GOOD);
    const auto result = to_data_change(1, dv.get());
    EXPECT_FALSE(result.change.value.has_value());
    EXPECT_EQ(result.change.quality, DataQuality::Bad);
    EXPECT_EQ(result.change.status_code, UA_STATUSCODE_GOOD);
    EXPECT_FALSE(result.conversion_failure.empty());
}

TEST(ToDataChange, NoValueWithGoodStatusIsLocalFailure)
{
    DataValue dv;
    dv.status(UA_STATUSCODE_GOOD);
    const auto result = to_data_change(1, dv.get());
    EXPECT_FALSE(result.change.value.has_value());
    EXPECT_EQ(result.change.quality, DataQuality::Bad);
    EXPECT_EQ(result.change.status_code, UA_STATUSCODE_GOOD);
    EXPECT_EQ(result.conversion_failure, "no value despite non-Bad status");
}

TEST(ToDataChange, AbsentOrZeroSourceTimestampStaysAbsent)
{
    DataValue without;
    without.double_value(1.0);
    EXPECT_FALSE(to_data_change(1, without.get()).change.source_timestamp.has_value());

    DataValue zero;
    zero.double_value(1.0).source_timestamp(0);
    const auto result = to_data_change(1, zero.get());
    EXPECT_FALSE(result.change.source_timestamp.has_value());
    EXPECT_FALSE(result.source_timestamp_out_of_range);
}

TEST(ToDataChange, OutOfRangeSourceTimestampIsFlaggedAndValueKept)
{
    DataValue dv;
    dv.double_value(1.0).source_timestamp(std::numeric_limits<UA_DateTime>::max());
    const auto result = to_data_change(1, dv.get());
    EXPECT_FALSE(result.change.source_timestamp.has_value());
    EXPECT_TRUE(result.source_timestamp_out_of_range);
    EXPECT_EQ(result.change.value, std::optional<TelemetryValue>{1.0});
    EXPECT_EQ(result.change.quality, DataQuality::Good);
}

} // namespace
} // namespace ieg::opcua::detail
