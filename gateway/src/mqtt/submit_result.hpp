#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <string_view>

namespace ieg::mqtt::detail {

// Maps a non-success return code of MQTTAsync_sendMessage() (the publication was NOT submitted)
// to a PublishResult:
//   MQTTASYNC_DISCONNECTED                       -> NotConnected (no usable connection)
//   invalid topic/payload/QoS/options            -> InvalidPoint (Permanent)
//   any other code (e.g. too many in flight)      -> Unavailable  (transient; Retryable)
// `reason` is the library's text for the code.
[[nodiscard]] output::PublishResult submit_failure_result(int rc, std::string_view reason);

} // namespace ieg::mqtt::detail
