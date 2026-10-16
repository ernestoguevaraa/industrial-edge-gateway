#pragma once

#include "ieg/opcua/opcua_client.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// open62541-backed IOpcUaClient. open62541 is a private implementation detail: this header
// must never include open62541 headers or expose its types.
namespace ieg::opcua {

struct Open62541ClientOptions
{
    // "opc.tcp://host:port[/path]"
    std::string endpoint;
    // Requested subscription publishing interval; also used as the sampling interval.
    std::chrono::milliseconds publishing_interval{1000};
    // Timeout for connect and service calls. A stop() arriving during a connection attempt can
    // wait up to about this long plus open62541's client shutdown (typically under one second).
    std::chrono::milliseconds request_timeout{2000};
    // Reconnect delay after n consecutive failures: min(initial * 2^n, max).
    std::chrono::milliseconds reconnect_initial_delay{500};
    std::chrono::milliseconds reconnect_max_delay{30000};
};

// Implements the IOpcUaClient contract with one adapter-owned worker thread that exclusively
// owns the open62541 client. Every connection attempt uses a fresh open62541 client, so a
// reconnect always recreates the session, the subscription and all monitored items.
class Open62541Client final : public IOpcUaClient
{
public:
    // Validates the options without any network activity; throws std::invalid_argument.
    explicit Open62541Client(Open62541ClientOptions options);
    ~Open62541Client() override;

    Open62541Client(const Open62541Client&) = delete;
    Open62541Client& operator=(const Open62541Client&) = delete;
    Open62541Client(Open62541Client&&) = delete;
    Open62541Client& operator=(Open62541Client&&) = delete;

    void start(std::vector<MonitoredItem> items, ClientCallbacks callbacks) override;

    // Calling stop() from inside a callback violates the IOpcUaClient precondition. It is
    // detected to avoid a self-join deadlock: it is logged as critical and shutdown is
    // requested, but that call returns without the usual guarantee (the current callback is
    // still running). The owner must still call stop() from another thread (or destroy the
    // client) to obtain the guarantee.
    void stop() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ieg::opcua
