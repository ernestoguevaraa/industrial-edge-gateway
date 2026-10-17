#include "mqtt/submit_result.hpp"

#include <MQTTAsync.h>
#include <gtest/gtest.h>

namespace ieg::mqtt::detail {
namespace {

using output::PublishStatus;
using output::RetryDisposition;

TEST(MqttSubmitResult, DisconnectedIsNotConnected)
{
    const auto result = submit_failure_result(MQTTASYNC_DISCONNECTED, "Client disconnected");
    EXPECT_EQ(result.status, PublishStatus::NotConnected);
    EXPECT_EQ(result.retry, RetryDisposition::Retryable);
}

TEST(MqttSubmitResult, TransientSubmissionFailuresAreUnavailable)
{
    for (const int rc :
         {MQTTASYNC_MAX_MESSAGES_INFLIGHT, MQTTASYNC_NO_MORE_MSGIDS,
          MQTTASYNC_MAX_BUFFERED_MESSAGES, MQTTASYNC_FAILURE, MQTTASYNC_OPERATION_INCOMPLETE}) {
        SCOPED_TRACE(rc);
        const auto result = submit_failure_result(rc, "reason");
        EXPECT_EQ(result.status, PublishStatus::Unavailable);
        EXPECT_EQ(result.retry, RetryDisposition::Retryable);
        EXPECT_EQ(result.detail, "publish not submitted: reason");
    }
}

TEST(MqttSubmitResult, InvalidRequestsAreInvalidPoint)
{
    for (const int rc : {MQTTASYNC_BAD_UTF8_STRING, MQTTASYNC_TOPICNAME_TRUNCATED,
                         MQTTASYNC_BAD_QOS, MQTTASYNC_BAD_MQTT_OPTION, MQTTASYNC_NULL_PARAMETER}) {
        SCOPED_TRACE(rc);
        const auto result = submit_failure_result(rc, "reason");
        EXPECT_EQ(result.status, PublishStatus::InvalidPoint);
        EXPECT_EQ(result.retry, RetryDisposition::Permanent);
    }
}

} // namespace
} // namespace ieg::mqtt::detail
