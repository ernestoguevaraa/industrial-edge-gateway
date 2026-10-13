#include "ieg/domain/telemetry_json.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ieg::domain {
namespace {

using namespace std::chrono;
using namespace std::chrono_literals;

Timestamp at_2026_10_13(nanoseconds time_of_day)
{
    return Timestamp{sys_days{2026y / October / 13}} + time_of_day;
}

TelemetryPoint make_point(TelemetryValue value)
{
    return TelemetryPoint{
        .asset = "site01.line01.oven01",
        .tag = "temperature",
        .value = std::move(value),
        .unit = "degC",
        .quality = DataQuality::Good,
        .source_timestamp = at_2026_10_13(16h + 10min + 27s + 242ms),
        .edge_timestamp = at_2026_10_13(16h + 10min + 27s + 249ms),
        .protocol = "opcua",
    };
}

TEST(TelemetryPoint, DefaultsToBadQualityWithoutSourceTimestamp)
{
    const TelemetryPoint point;
    EXPECT_EQ(point.quality, DataQuality::Bad);
    EXPECT_FALSE(point.source_timestamp.has_value());
}

TEST(FormatTimestamp, FormatsEpoch)
{
    EXPECT_EQ(format_timestamp(Timestamp{}), "1970-01-01T00:00:00.000Z");
}

TEST(FormatTimestamp, TruncatesToMilliseconds)
{
    EXPECT_EQ(format_timestamp(at_2026_10_13(16h + 10min + 27s + 242'999'999ns)),
              "2026-10-13T16:10:27.242Z");
}

TEST(FormatTimestamp, TruncatesTowardsThePastBeforeEpoch)
{
    EXPECT_EQ(format_timestamp(Timestamp{-1ns}), "1969-12-31T23:59:59.999Z");
}

TEST(TelemetryJson, SerializesAllFieldsInDeterministicOrder)
{
    EXPECT_EQ(to_json(make_point(842.4)).dump(),
              R"({"asset":"site01.line01.oven01","tag":"temperature","value":842.4,"unit":"degC",)"
              R"("quality":"GOOD","source_timestamp":"2026-10-13T16:10:27.242Z",)"
              R"("edge_timestamp":"2026-10-13T16:10:27.249Z","protocol":"opcua"})");
}

TEST(TelemetryJson, EqualPointsProduceIdenticalOutput)
{
    EXPECT_EQ(to_json(make_point(842.4)).dump(), to_json(make_point(842.4)).dump());
}

TEST(TelemetryJson, SerializesMissingSourceTimestampAsNull)
{
    auto point = make_point(842.4);
    point.source_timestamp.reset();

    const auto json = to_json(point);
    ASSERT_TRUE(json.contains("source_timestamp"));
    EXPECT_TRUE(json.at("source_timestamp").is_null());
}

TEST(TelemetryJson, SerializesQuality)
{
    auto point = make_point(842.4);
    point.quality = DataQuality::Uncertain;
    EXPECT_EQ(to_json(point).at("quality"), "UNCERTAIN");
}

TEST(TelemetryJson, SerializesBoolValue)
{
    const auto value = to_json(make_point(true)).at("value");
    EXPECT_TRUE(value.is_boolean());
    EXPECT_EQ(value.dump(), "true");
}

TEST(TelemetryJson, SerializesSignedIntegerValue)
{
    const auto value = to_json(make_point(std::numeric_limits<std::int64_t>::min())).at("value");
    EXPECT_TRUE(value.is_number_integer());
    EXPECT_EQ(value.dump(), "-9223372036854775808");
}

TEST(TelemetryJson, SerializesUnsignedIntegerValue)
{
    const auto value = to_json(make_point(std::numeric_limits<std::uint64_t>::max())).at("value");
    EXPECT_TRUE(value.is_number_unsigned());
    EXPECT_EQ(value.dump(), "18446744073709551615");
}

TEST(TelemetryJson, SerializesDoubleValue)
{
    const auto value = to_json(make_point(0.1)).at("value");
    EXPECT_TRUE(value.is_number_float());
    EXPECT_EQ(value.dump(), "0.1");
}

TEST(TelemetryJson, SerializesStringValue)
{
    const auto value = to_json(make_point(std::string{"Running"})).at("value");
    EXPECT_TRUE(value.is_string());
    EXPECT_EQ(value.dump(), R"("Running")");
}

TEST(TelemetryJson, RejectsNaN)
{
    EXPECT_THROW((void)to_json(make_point(std::numeric_limits<double>::quiet_NaN())),
                 std::domain_error);
}

TEST(TelemetryJson, RejectsPositiveInfinity)
{
    EXPECT_THROW((void)to_json(make_point(std::numeric_limits<double>::infinity())),
                 std::domain_error);
}

TEST(TelemetryJson, RejectsNegativeInfinity)
{
    EXPECT_THROW((void)to_json(make_point(-std::numeric_limits<double>::infinity())),
                 std::domain_error);
}

TEST(TelemetryJson, NonFiniteErrorIdentifiesAssetAndTag)
{
    try {
        (void)to_json(make_point(std::numeric_limits<double>::infinity()));
        FAIL() << "expected std::domain_error";
    } catch (const std::domain_error& error) {
        EXPECT_STREQ(error.what(), "cannot serialize non-finite value inf for asset "
                                   "'site01.line01.oven01' tag 'temperature' to JSON");
    }
}

} // namespace
} // namespace ieg::domain
