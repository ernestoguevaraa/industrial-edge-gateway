#include "ieg/output/telemetry_publisher.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace ieg::output {
namespace {

TEST(PublishResult, DefaultDispositionsPerStatus)
{
    EXPECT_EQ(PublishResult::of(PublishStatus::Delivered).retry, RetryDisposition::NotApplicable);
    EXPECT_EQ(PublishResult::of(PublishStatus::NotConnected).retry, RetryDisposition::Retryable);
    EXPECT_EQ(PublishResult::of(PublishStatus::Timeout).retry, RetryDisposition::Retryable);
    EXPECT_EQ(PublishResult::of(PublishStatus::Stopped).retry, RetryDisposition::Retryable);
    EXPECT_EQ(PublishResult::of(PublishStatus::InvalidPoint).retry, RetryDisposition::Permanent);
}

TEST(PublishResult, RejectedNeedsAnExplicitDisposition)
{
    EXPECT_THROW(static_cast<void>(PublishResult::of(PublishStatus::Rejected)),
                 std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(PublishResult::rejected(RetryDisposition::NotApplicable, "reason")),
        std::invalid_argument);

    const auto quota = PublishResult::rejected(RetryDisposition::Retryable, "quota exceeded");
    EXPECT_EQ(quota.status, PublishStatus::Rejected);
    EXPECT_TRUE(quota.retryable());
    EXPECT_EQ(quota.detail, "quota exceeded");

    const auto denied = PublishResult::rejected(RetryDisposition::Permanent, "not authorized");
    EXPECT_EQ(denied.status, PublishStatus::Rejected);
    EXPECT_FALSE(denied.retryable());
}

TEST(PublishResult, DeliveredAndRetryableAreIndependent)
{
    const auto delivered = PublishResult::of(PublishStatus::Delivered);
    EXPECT_TRUE(delivered.delivered());
    EXPECT_FALSE(delivered.retryable());

    const auto timeout = PublishResult::of(PublishStatus::Timeout, "no PUBACK");
    EXPECT_FALSE(timeout.delivered());
    EXPECT_TRUE(timeout.retryable());
    EXPECT_EQ(timeout.detail, "no PUBACK");

    const auto invalid = PublishResult::of(PublishStatus::InvalidPoint);
    EXPECT_FALSE(invalid.delivered());
    EXPECT_FALSE(invalid.retryable());
}

TEST(PublishResult, ToStringNamesEveryValue)
{
    EXPECT_EQ(to_string(PublishStatus::Delivered), "Delivered");
    EXPECT_EQ(to_string(PublishStatus::NotConnected), "NotConnected");
    EXPECT_EQ(to_string(PublishStatus::Timeout), "Timeout");
    EXPECT_EQ(to_string(PublishStatus::Rejected), "Rejected");
    EXPECT_EQ(to_string(PublishStatus::InvalidPoint), "InvalidPoint");
    EXPECT_EQ(to_string(PublishStatus::Stopped), "Stopped");
    EXPECT_EQ(to_string(RetryDisposition::NotApplicable), "NotApplicable");
    EXPECT_EQ(to_string(RetryDisposition::Retryable), "Retryable");
    EXPECT_EQ(to_string(RetryDisposition::Permanent), "Permanent");
}

static_assert(to_string(PublishStatus::Delivered) == "Delivered");

} // namespace
} // namespace ieg::output
