// Lifecycle tests for the real adapter without an OPC UA server: endpoints are local loopback
// sockets that are either closed or accept TCP but never answer.

#include "ieg/opcua/open62541_client.hpp"
#include "opcua/callback_recorder.hpp"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ieg::opcua {
namespace {

using namespace std::chrono_literals;
using testing::CallbackRecorder;
using Clock = std::chrono::steady_clock;

// A loopback TCP socket. listening=true accepts connections (kernel backlog) but never answers;
// listening=false is closed after reserving the port, so connections are refused.
class LoopbackPort
{
public:
    explicit LoopbackPort(bool listening) : fd_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        // NOLINTNEXTLINE(*-reinterpret-cast): BSD sockets API.
        auto* generic = reinterpret_cast<sockaddr*>(&address);
        socklen_t length = sizeof(address);
        if (fd_ < 0 || ::bind(fd_, generic, length) != 0 ||
            ::getsockname(fd_, generic, &length) != 0 || (listening && ::listen(fd_, 4) != 0)) {
            throw std::runtime_error("could not prepare loopback socket");
        }
        port_ = ntohs(address.sin_port);
        if (!listening) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~LoopbackPort()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    LoopbackPort(const LoopbackPort&) = delete;
    LoopbackPort& operator=(const LoopbackPort&) = delete;
    LoopbackPort(LoopbackPort&&) = delete;
    LoopbackPort& operator=(LoopbackPort&&) = delete;

    [[nodiscard]] std::string endpoint() const
    {
        return "opc.tcp://127.0.0.1:" + std::to_string(port_);
    }

private:
    int fd_{-1};
    std::uint16_t port_{0};
};

Open62541ClientOptions options_for(const std::string& endpoint)
{
    return Open62541ClientOptions{.endpoint = endpoint,
                                  .publishing_interval = 100ms,
                                  .request_timeout = 1000ms,
                                  .reconnect_initial_delay = 20ms,
                                  .reconnect_max_delay = 80ms};
}

std::vector<MonitoredItem> one_item()
{
    return {{1, "ns=2;s=Line01.Press01.Pressure"}};
}

ClientCallbacks noop_callbacks()
{
    return ClientCallbacks{.on_data_change = [](const DataChange&) {},
                           .on_status_change = [](const ConnectionStatus&) {}};
}

// Statuses must strictly alternate Connecting, Disconnected(reason), Connecting, ...
::testing::AssertionResult
alternates_connecting_disconnected(const std::vector<ConnectionStatus>& statuses)
{
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto expected =
            i % 2 == 0 ? ConnectionState::Connecting : ConnectionState::Disconnected;
        if (statuses[i].state != expected) {
            return ::testing::AssertionFailure() << "unexpected state at index " << i;
        }
        if (expected == ConnectionState::Disconnected && statuses[i].detail.empty()) {
            return ::testing::AssertionFailure() << "empty Disconnected detail at index " << i;
        }
    }
    return ::testing::AssertionSuccess();
}

std::string invalid_argument_message(const std::function<void()>& action)
{
    try {
        action();
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    ADD_FAILURE() << "expected std::invalid_argument";
    return {};
}

// ---- Validation ------------------------------------------------------------------------

TEST(Open62541Client, RejectsInvalidOptions)
{
    auto options = options_for("http://127.0.0.1:4840");
    options.publishing_interval = 0ms;
    options.reconnect_max_delay = 1ms;
    const auto message = invalid_argument_message([&] { const Open62541Client client{options}; });
    EXPECT_NE(message.find("opc.tcp://"), std::string::npos);
    EXPECT_NE(message.find("publishing_interval"), std::string::npos);
    EXPECT_NE(message.find("reconnect_max_delay"), std::string::npos);
}

TEST(Open62541Client, RejectsInvalidItemsListingEveryProblem)
{
    Open62541Client client{options_for("opc.tcp://127.0.0.1:1")};
    const auto message = invalid_argument_message([&] {
        client.start({{1, "ns=2;s=Ok"}, {1, "ns=2;s=Dup"}, {2, ""}, {3, "ns=2;x=bad"}},
                     noop_callbacks());
    });
    EXPECT_NE(message.find("duplicate item id 1"), std::string::npos);
    EXPECT_NE(message.find("item 2: empty node_id"), std::string::npos);
    EXPECT_NE(message.find("item 3: invalid node_id"), std::string::npos);

    EXPECT_FALSE(invalid_argument_message([&] { client.start({}, noop_callbacks()); }).empty());
    EXPECT_FALSE(
        invalid_argument_message([&] { client.start(one_item(), ClientCallbacks{}); }).empty());
}

TEST(Open62541Client, SecondStartIsLogicError)
{
    const LoopbackPort closed{false};
    Open62541Client client{options_for(closed.endpoint())};
    client.start(one_item(), noop_callbacks());
    EXPECT_THROW(client.start(one_item(), noop_callbacks()), std::logic_error);
    client.stop();
}

TEST(Open62541Client, StopIsIdempotentAndSafeWithoutStart)
{
    Open62541Client client{options_for("opc.tcp://127.0.0.1:1")};
    client.stop();
    client.stop();
}

TEST(Open62541Client, DestructorWithoutStartOrStop)
{
    const LoopbackPort closed{false};
    {
        const Open62541Client never_started{options_for(closed.endpoint())};
    }
    CallbackRecorder recorder;
    {
        Open62541Client running{options_for(closed.endpoint())};
        running.start(one_item(), recorder.callbacks());
    } // destructor stops
    const auto calls = recorder.total_calls();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(recorder.total_calls(), calls);
}

// ---- Connection lifecycle --------------------------------------------------------------

TEST(Open62541Client, RefusedConnectionRetriesWithStatusSequence)
{
    const LoopbackPort closed{false};
    CallbackRecorder recorder;
    Open62541Client client{options_for(closed.endpoint())};
    client.start(one_item(), recorder.callbacks());

    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Disconnected) >= 3;
        },
        5s));
    client.stop();

    const auto statuses = recorder.statuses();
    ASSERT_GE(statuses.size(), 6U);
    EXPECT_TRUE(alternates_connecting_disconnected(statuses));
    EXPECT_TRUE(recorder.data().empty());
    EXPECT_FALSE(recorder.overlap_detected());
}

TEST(Open62541Client, StartReturnsWithoutWaitingForConnectivity)
{
    const LoopbackPort silent{true}; // TCP accepted, OPC UA handshake never answered
    CallbackRecorder recorder;
    Open62541Client client{options_for(silent.endpoint())};

    const auto before = Clock::now();
    client.start(one_item(), recorder.callbacks());
    EXPECT_LT(Clock::now() - before, 100ms);

    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) { return !r.statuses_unlocked().empty(); }, 2s));
    client.stop();
}

TEST(Open62541Client, StopDuringBlockedConnectEmitsNoDisconnected)
{
    const LoopbackPort silent{true};
    CallbackRecorder recorder;
    Open62541Client client{options_for(silent.endpoint())}; // request_timeout 1 s
    client.start(one_item(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) { return !r.statuses_unlocked().empty(); }, 2s));

    const auto before = Clock::now();
    client.stop(); // the worker is inside the blocking connect
    // Bounded by request_timeout (1 s) plus open62541's shutdown of the half-open socket.
    EXPECT_LT(Clock::now() - before, 2500ms);

    const auto statuses = recorder.statuses();
    ASSERT_EQ(statuses.size(), 1U);
    EXPECT_EQ(statuses[0].state, ConnectionState::Connecting);
    EXPECT_EQ(recorder.active_callbacks(), 0);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(recorder.total_calls(), 1U);
}

TEST(Open62541Client, StopDuringBackoffReturnsPromptlyAndSilently)
{
    const LoopbackPort closed{false};
    auto options = options_for(closed.endpoint());
    options.reconnect_initial_delay = 30s;
    options.reconnect_max_delay = 30s;
    CallbackRecorder recorder;
    Open62541Client client{options};
    client.start(one_item(), recorder.callbacks());
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Disconnected) == 1;
        },
        5s));

    const auto before = Clock::now();
    client.stop();
    EXPECT_LT(Clock::now() - before, 500ms);

    const auto calls = recorder.total_calls();
    EXPECT_EQ(calls, 2U); // Connecting, Disconnected(refused) - nothing from stop()
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(recorder.total_calls(), calls);
    EXPECT_EQ(recorder.active_callbacks(), 0);
}

TEST(Open62541Client, ThrowingCallbackDoesNotStopTheWorker)
{
    const LoopbackPort closed{false};
    CallbackRecorder recorder;
    Open62541Client client{options_for(closed.endpoint())};
    client.start(one_item(), recorder.callbacks([](const ConnectionStatus&) {
        throw std::runtime_error("callback failure");
    }));
    EXPECT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Connecting) >= 3;
        },
        5s));
    client.stop();
}

TEST(Open62541Client, StopFromCallbackIsDetectedWithoutDeadlock)
{
    const LoopbackPort closed{false};
    CallbackRecorder recorder;
    Open62541Client client{options_for(closed.endpoint())};
    client.start(one_item(), recorder.callbacks([&client](const ConnectionStatus& status) {
        if (status.state == ConnectionState::Disconnected) {
            client.stop(); // precondition violation: must not deadlock
        }
    }));
    ASSERT_TRUE(recorder.wait_until(
        [](const CallbackRecorder& r) {
            return r.count_state_unlocked(ConnectionState::Disconnected) >= 1;
        },
        5s));
    // A valid stop() from the owner thread still provides the full guarantee.
    client.stop();
    const auto calls = recorder.total_calls();
    EXPECT_EQ(recorder.active_callbacks(), 0);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(recorder.total_calls(), calls);
    EXPECT_EQ(recorder.count_state(ConnectionState::Disconnected), 1U);
}

} // namespace
} // namespace ieg::opcua
