#include "mqtt/paho_trace.hpp"

#include <MQTTAsync.h>

namespace ieg::mqtt::detail {

TraceSeverity paho_trace_severity(int paho_level) noexcept
{
    if (paho_level >= MQTTASYNC_TRACE_SEVERE) {
        return TraceSeverity::Error;
    }
    if (paho_level >= MQTTASYNC_TRACE_ERROR) {
        return TraceSeverity::Warning;
    }
    return TraceSeverity::Debug;
}

} // namespace ieg::mqtt::detail
