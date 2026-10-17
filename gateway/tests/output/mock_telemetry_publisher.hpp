#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <gmock/gmock.h>

namespace ieg::output::testing {

class MockTelemetryPublisher : public ITelemetryPublisher
{
public:
    MOCK_METHOD(void, start, (PublisherStatusCallback on_status), (override));
    MOCK_METHOD(PublishResult, publish, (const domain::TelemetryPoint& point), (override));
    // gmock's generated body can throw on expectation failures; that only aborts a test run.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    MOCK_METHOD(void, stop, (), (noexcept, override));
};

} // namespace ieg::output::testing
