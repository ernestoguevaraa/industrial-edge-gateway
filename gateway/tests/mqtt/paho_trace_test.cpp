#include "mqtt/paho_trace.hpp"

#include <MQTTAsync.h>
#include <gtest/gtest.h>

namespace ieg::mqtt::detail {
namespace {

TEST(PahoTraceSeverity, InformationalTraceIncludingTheStartupBannerIsDebug)
{
    // Paho writes its "Trace Output" banner at TRACE_MINIMUM even when the configured trace
    // level is ERROR.
    for (const int level : {MQTTASYNC_TRACE_MAXIMUM, MQTTASYNC_TRACE_MEDIUM,
                            MQTTASYNC_TRACE_MINIMUM, MQTTASYNC_TRACE_PROTOCOL}) {
        EXPECT_EQ(paho_trace_severity(level), TraceSeverity::Debug) << level;
    }
}

TEST(PahoTraceSeverity, GenuineErrorsKeepTheirSeverity)
{
    EXPECT_EQ(paho_trace_severity(MQTTASYNC_TRACE_ERROR), TraceSeverity::Warning);
    EXPECT_EQ(paho_trace_severity(MQTTASYNC_TRACE_SEVERE), TraceSeverity::Error);
    EXPECT_EQ(paho_trace_severity(MQTTASYNC_TRACE_FATAL), TraceSeverity::Error);
}

} // namespace
} // namespace ieg::mqtt::detail
