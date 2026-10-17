#include "ieg/output/telemetry_publisher.hpp"
#include "output/mock_telemetry_publisher.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <type_traits>
#include <utility>
#include <vector>

namespace ieg::output {
namespace {

using ::testing::_;
using ::testing::Field;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::StrictMock;

static_assert(std::is_abstract_v<ITelemetryPublisher>);
static_assert(std::has_virtual_destructor_v<ITelemetryPublisher>);
static_assert(!std::is_copy_constructible_v<ITelemetryPublisher>);
static_assert(!std::is_move_constructible_v<ITelemetryPublisher>);
static_assert(noexcept(std::declval<ITelemetryPublisher&>().stop()));

domain::TelemetryPoint point(const char* tag, double value)
{
    domain::TelemetryPoint p;
    p.asset = "site01.line01.press01";
    p.tag = tag;
    p.value = value;
    p.unit = "bar";
    p.quality = domain::DataQuality::Good;
    p.protocol = "opcua";
    return p;
}

TEST(TelemetryPublisherMock, ScriptedPublishResultsReachTheCaller)
{
    StrictMock<testing::MockTelemetryPublisher> mock;
    EXPECT_CALL(mock, publish(Field(&domain::TelemetryPoint::tag, "pressure")))
        .WillOnce(Return(PublishResult::of(PublishStatus::Delivered)))
        .WillOnce(Return(PublishResult::of(PublishStatus::NotConnected)))
        .WillOnce(Return(PublishResult::of(PublishStatus::Timeout, "no PUBACK")))
        .WillOnce(Return(PublishResult::rejected(RetryDisposition::Permanent, "not authorized")));

    // The consumer only sees the abstract interface.
    ITelemetryPublisher& publisher = mock;
    std::vector<PublishResult> results;
    results.reserve(4);
    for (int i = 0; i < 4; ++i) {
        results.push_back(publisher.publish(point("pressure", 180.0 + i)));
    }
    EXPECT_TRUE(results[0].delivered());
    EXPECT_EQ(results[1].status, PublishStatus::NotConnected);
    EXPECT_TRUE(results[1].retryable());
    EXPECT_EQ(results[2].detail, "no PUBACK");
    EXPECT_EQ(results[3].status, PublishStatus::Rejected);
    EXPECT_FALSE(results[3].retryable());
}

TEST(TelemetryPublisherMock, CapturedStatusCallbackDeliversTransitions)
{
    StrictMock<testing::MockTelemetryPublisher> mock;
    PublisherStatusCallback captured;
    EXPECT_CALL(mock, start(_)).WillOnce(SaveArg<0>(&captured));
    EXPECT_CALL(mock, stop()).Times(1);

    std::vector<PublisherStatus> statuses;
    ITelemetryPublisher& publisher = mock;
    publisher.start([&statuses](const PublisherStatus& s) { statuses.push_back(s); });
    ASSERT_TRUE(captured);

    captured({PublisherState::Connecting, ""});
    captured({PublisherState::Connected, ""});
    captured({PublisherState::Disconnected, "connection lost"});
    publisher.stop();

    EXPECT_EQ(statuses,
              (std::vector<PublisherStatus>{{PublisherState::Connecting, ""},
                                            {PublisherState::Connected, ""},
                                            {PublisherState::Disconnected, "connection lost"}}));
}

} // namespace
} // namespace ieg::output
