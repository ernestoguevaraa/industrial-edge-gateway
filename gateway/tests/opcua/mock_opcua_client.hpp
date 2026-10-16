#pragma once

#include "ieg/opcua/opcua_client.hpp"

#include <gmock/gmock.h>

#include <vector>

namespace ieg::opcua::testing {

class MockOpcUaClient : public IOpcUaClient
{
public:
    MOCK_METHOD(void, start, (std::vector<MonitoredItem> items, ClientCallbacks callbacks),
                (override));
    // gmock's generated body can throw on expectation failures; that only aborts a test run.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    MOCK_METHOD(void, stop, (), (noexcept, override));
};

} // namespace ieg::opcua::testing
