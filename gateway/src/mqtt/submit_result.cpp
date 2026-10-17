#include "mqtt/submit_result.hpp"

#include <MQTTAsync.h>

#include <string>

namespace ieg::mqtt::detail {

output::PublishResult submit_failure_result(int rc, std::string_view reason)
{
    using output::PublishResult;
    using output::PublishStatus;
    switch (rc) {
    case MQTTASYNC_DISCONNECTED:
        return PublishResult::of(PublishStatus::NotConnected, "not connected to broker");
    case MQTTASYNC_BAD_UTF8_STRING:
    case MQTTASYNC_TOPICNAME_TRUNCATED:
    case MQTTASYNC_BAD_QOS:
    case MQTTASYNC_BAD_MQTT_OPTION:
    case MQTTASYNC_NULL_PARAMETER:
        return PublishResult::of(PublishStatus::InvalidPoint,
                                 "publish rejected: " + std::string{reason});
    default:
        // Not submitted although the client may still be connected (e.g. too many messages in
        // flight, no free packet identifier, out of memory); a later attempt may succeed.
        return PublishResult::of(PublishStatus::Unavailable,
                                 "publish not submitted: " + std::string{reason});
    }
}

} // namespace ieg::mqtt::detail
