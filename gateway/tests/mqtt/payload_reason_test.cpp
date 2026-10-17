#include "ieg/domain/telemetry_json.hpp"
#include "mqtt/payload.hpp"
#include "mqtt/reason_codes.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace ieg::mqtt::detail {
namespace {

using output::PublishStatus;
using output::RetryDisposition;

domain::TelemetryPoint sample_point()
{
    domain::TelemetryPoint point;
    point.asset = "site01.line01.oven01";
    point.tag = "temperature";
    point.value = 842.4;
    point.unit = "degC";
    point.quality = domain::DataQuality::Good;
    point.protocol = "opcua";
    return point;
}

// ---- Payload -------------------------------------------------------------------------------

TEST(MqttPayload, IsTheCompactDomainJson)
{
    const auto point = sample_point();
    const auto payload = encode_payload(point);
    ASSERT_TRUE(payload.ok()) << payload.error;
    EXPECT_EQ(payload.json, domain::to_json(point).dump());
    EXPECT_EQ(payload.json.find('\n'), std::string::npos);
}

TEST(MqttPayload, NonFiniteDoubleIsAnError)
{
    auto point = sample_point();
    point.value = std::numeric_limits<double>::quiet_NaN();
    const auto payload = encode_payload(point);
    EXPECT_FALSE(payload.ok());
    EXPECT_TRUE(payload.json.empty());
}

TEST(MqttPayload, InvalidUtf8StringIsAnError)
{
    auto point = sample_point();
    point.value = std::string{"\xFF\xFE"};
    EXPECT_FALSE(encode_payload(point).ok());
}

// ---- PUBACK reason codes --------------------------------------------------------------------

TEST(MqttPubackReason, OnlySuccessAndNoMatchingSubscribersAreDelivered)
{
    for (const int code : {0x00, 0x10}) {
        SCOPED_TRACE(code);
        const auto mapping = map_puback_reason(code);
        EXPECT_EQ(mapping.result.status, PublishStatus::Delivered);
        EXPECT_EQ(mapping.result.retry, RetryDisposition::NotApplicable);
        EXPECT_FALSE(mapping.unexpected);
    }
}

TEST(MqttPubackReason, UndefinedCodesBelow0x80AreNotTreatedAsSuccess)
{
    for (const int code : {0x01, 0x04, 0x11, 0x7F}) {
        SCOPED_TRACE(code);
        const auto mapping = map_puback_reason(code);
        EXPECT_EQ(mapping.result.status, PublishStatus::Rejected);
        EXPECT_TRUE(mapping.result.retryable());
        EXPECT_TRUE(mapping.unexpected);
    }
}

TEST(MqttPubackReason, PermanentRejections)
{
    for (const int code : {0x87, 0x90, 0x99}) {
        SCOPED_TRACE(code);
        const auto mapping = map_puback_reason(code);
        EXPECT_EQ(mapping.result.status, PublishStatus::Rejected);
        EXPECT_EQ(mapping.result.retry, RetryDisposition::Permanent);
        EXPECT_FALSE(mapping.unexpected);
    }
}

TEST(MqttPubackReason, RetryableRejections)
{
    for (const int code : {0x80, 0x83, 0x91, 0x97}) {
        SCOPED_TRACE(code);
        const auto mapping = map_puback_reason(code);
        EXPECT_EQ(mapping.result.status, PublishStatus::Rejected);
        EXPECT_EQ(mapping.result.retry, RetryDisposition::Retryable);
        EXPECT_FALSE(mapping.unexpected);
    }
}

TEST(MqttPubackReason, UnexpectedCodesAreRetryableAndFlagged)
{
    for (const int code : {0x81, 0x8E, 0x9F, 0xFF}) {
        SCOPED_TRACE(code);
        const auto mapping = map_puback_reason(code);
        EXPECT_EQ(mapping.result.status, PublishStatus::Rejected);
        EXPECT_EQ(mapping.result.retry, RetryDisposition::Retryable);
        EXPECT_TRUE(mapping.unexpected);
    }
}

TEST(MqttPubackReason, DetailNamesCodeAndBrokerReasonString)
{
    const auto mapping = map_puback_reason(0x87, "ACL denies forbidden/#");
    EXPECT_EQ(mapping.result.detail, "PUBACK 0x87 Not authorized: ACL denies forbidden/#");
    EXPECT_EQ(map_puback_reason(0x97).result.detail, "PUBACK 0x97 Quota exceeded");
}

} // namespace
} // namespace ieg::mqtt::detail
