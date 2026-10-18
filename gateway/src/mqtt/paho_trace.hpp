#pragma once

#include <cstdint>

namespace ieg::mqtt::detail {

enum class TraceSeverity : std::uint8_t
{
    Debug,
    Warning,
    Error
};

// Severity for a line from Paho's trace hook, given its MQTTASYNC_TRACE_LEVELS value.
// Below MQTTASYNC_TRACE_ERROR the line is informational trace output (e.g. the library's
// "Trace Output" startup banner, which Paho emits at TRACE_MINIMUM regardless of the configured
// trace level): Debug. ERROR stays a Warning; SEVERE and FATAL are Errors.
[[nodiscard]] TraceSeverity paho_trace_severity(int paho_level) noexcept;

} // namespace ieg::mqtt::detail
