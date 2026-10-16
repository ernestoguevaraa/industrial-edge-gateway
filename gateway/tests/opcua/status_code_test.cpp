#include "ieg/opcua/opcua_client.hpp"

#include <gtest/gtest.h>

namespace ieg::opcua {
namespace {

using domain::DataQuality;

// Values from OPC 10000-4 / the OPC Foundation StatusCode table.
constexpr StatusCode Good = 0x00000000U;
constexpr StatusCode GoodOverload = 0x002F0000U;
constexpr StatusCode UncertainLastUsableValue = 0x40900000U;
constexpr StatusCode UncertainSensorNotAccurate = 0x40930000U;
constexpr StatusCode BadNodeIdUnknown = 0x80340000U;
constexpr StatusCode BadSensorFailure = 0x808C0000U;
constexpr StatusCode BadConnectionClosed = 0x80AE0000U;

TEST(QualityFromStatusCode, MapsSeverityBoundaries)
{
    EXPECT_EQ(quality_from_status_code(0x00000000U), DataQuality::Good);
    EXPECT_EQ(quality_from_status_code(0x3FFFFFFFU), DataQuality::Good);
    EXPECT_EQ(quality_from_status_code(0x40000000U), DataQuality::Uncertain);
    EXPECT_EQ(quality_from_status_code(0x7FFFFFFFU), DataQuality::Uncertain);
    EXPECT_EQ(quality_from_status_code(0x80000000U), DataQuality::Bad);
    EXPECT_EQ(quality_from_status_code(0xBFFFFFFFU), DataQuality::Bad);
}

TEST(QualityFromStatusCode, TreatsReservedSeverityAsBad)
{
    // Severity 11 is reserved by the specification; never report it as usable.
    EXPECT_EQ(quality_from_status_code(0xC0000000U), DataQuality::Bad);
    EXPECT_EQ(quality_from_status_code(0xFFFFFFFFU), DataQuality::Bad);
}

TEST(QualityFromStatusCode, MapsRealStatusCodes)
{
    EXPECT_EQ(quality_from_status_code(Good), DataQuality::Good);
    EXPECT_EQ(quality_from_status_code(GoodOverload), DataQuality::Good);
    EXPECT_EQ(quality_from_status_code(UncertainLastUsableValue), DataQuality::Uncertain);
    EXPECT_EQ(quality_from_status_code(UncertainSensorNotAccurate), DataQuality::Uncertain);
    EXPECT_EQ(quality_from_status_code(BadNodeIdUnknown), DataQuality::Bad);
    EXPECT_EQ(quality_from_status_code(BadSensorFailure), DataQuality::Bad);
    EXPECT_EQ(quality_from_status_code(BadConnectionClosed), DataQuality::Bad);
}

TEST(QualityFromStatusCode, IgnoresSubCodeAndInfoBits)
{
    // Only bits 31..30 decide; sub-code and info bits (e.g. overflow, limit) do not.
    EXPECT_EQ(quality_from_status_code(Good | 0x00000400U), DataQuality::Good);
    EXPECT_EQ(quality_from_status_code(UncertainSensorNotAccurate | 0x00000100U),
              DataQuality::Uncertain);
}

static_assert(quality_from_status_code(0x40930000U) == DataQuality::Uncertain);

} // namespace
} // namespace ieg::opcua
