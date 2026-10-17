// Lifecycle tests for the real MqttPublisher without a broker (closed loopback port).

#include "ieg/mqtt/mqtt_publisher.hpp"
#include "mqtt/status_recorder.hpp"

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

namespace ieg::mqtt {
namespace {

using namespace std::chrono_literals;
using output::PublisherState;
using output::PublishStatus;
using testing::StatusRecorder;
using Clock = std::chrono::steady_clock;

// Reserves a loopback port and releases it, so connections to it are refused.
std::uint16_t closed_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(*-reinterpret-cast): BSD sockets API.
    auto* generic = reinterpret_cast<sockaddr*>(&address);
    socklen_t length = sizeof(address);
    if (fd < 0 || ::bind(fd, generic, length) != 0 || ::getsockname(fd, generic, &length) != 0) {
        throw std::runtime_error("could not reserve a loopback port");
    }
    ::close(fd);
    return ntohs(address.sin_port);
}

MqttPublisherOptions options_for(std::uint16_t port)
{
    return MqttPublisherOptions{.host = "127.0.0.1",
                                .port = port,
                                .topic_prefix = "industrial",
                                .client_id = "ieg-unit-test",
                                .publish_timeout = 1000ms,
                                .connect_timeout = 1000ms,
                                .keep_alive = 5s,
                                .reconnect_initial_delay = 20ms,
                                .reconnect_max_delay = 80ms};
}

domain::TelemetryPoint sample_point()
{
    domain::TelemetryPoint point;
    point.asset = "site01.line01.press01";
    point.tag = "pressure";
    point.value = 180.5;
    point.unit = "bar";
    point.quality = domain::DataQuality::Good;
    point.protocol = "opcua";
    return point;
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

TEST(MqttPublisher, RejectsInvalidOptionsListingEveryProblem)
{
    auto options = options_for(1883);
    options.host.clear();
    options.port = 0;
    options.topic_prefix = "industrial/";
    options.publish_timeout = 0ms;
    options.keep_alive = 0s;
    options.reconnect_max_delay = 1ms;
    const auto message = invalid_argument_message([&] { const MqttPublisher publisher{options}; });
    for (const char* expected :
         {"host", "port", "empty level", "publish_timeout", "keep_alive", "reconnect_max_delay"}) {
        EXPECT_NE(message.find(expected), std::string::npos) << expected;
    }
}

TEST(MqttPublisher, StartValidatesCallbackAndRejectsSecondStart)
{
    MqttPublisher publisher{options_for(closed_port())};
    EXPECT_THROW(publisher.start({}), std::invalid_argument);
    StatusRecorder recorder;
    publisher.start(recorder.callback());
    // A separate callback: re-arming the recorder would race with the running worker.
    EXPECT_THROW(publisher.start([](const output::PublisherStatus&) {}), std::logic_error);
    publisher.stop();
}

TEST(MqttPublisher, PublishBeforeStartAndAfterStopReturnsStopped)
{
    MqttPublisher publisher{options_for(closed_port())};
    EXPECT_EQ(publisher.publish(sample_point()).status, PublishStatus::Stopped);
    StatusRecorder recorder;
    publisher.start(recorder.callback());
    publisher.stop();
    const auto result = publisher.publish(sample_point());
    EXPECT_EQ(result.status, PublishStatus::Stopped);
    EXPECT_TRUE(result.retryable());
    EXPECT_THROW(publisher.start(recorder.callback()), std::logic_error);
}

TEST(MqttPublisher, StopIsIdempotentAndDestructorIsSafeWithoutStart)
{
    {
        const MqttPublisher never_started{options_for(1883)};
    }
    MqttPublisher publisher{options_for(closed_port())};
    publisher.stop();
    publisher.stop();
}

TEST(MqttPublisher, StartReturnsImmediatelyAndRetriesWithStatusSequence)
{
    MqttPublisher publisher{options_for(closed_port())};
    StatusRecorder recorder;
    const auto before = Clock::now();
    publisher.start(recorder.callback());
    EXPECT_LT(Clock::now() - before, 100ms);

    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Disconnected, 3, 10s));
    publisher.stop();
    const auto statuses = recorder.statuses();
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto expected =
            i % 2 == 0 ? PublisherState::Connecting : PublisherState::Disconnected;
        EXPECT_EQ(statuses[i].state, expected) << "index " << i;
    }
    EXPECT_FALSE(recorder.overlap_detected());
}

TEST(MqttPublisher, PublishWhileDisconnectedReturnsNotConnectedImmediately)
{
    MqttPublisher publisher{options_for(closed_port())};
    StatusRecorder recorder;
    publisher.start(recorder.callback());
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Disconnected, 1, 10s));

    const auto before = Clock::now();
    const auto result = publisher.publish(sample_point());
    EXPECT_LT(Clock::now() - before, 50ms);
    EXPECT_EQ(result.status, PublishStatus::NotConnected);
    EXPECT_TRUE(result.retryable());
    publisher.stop();
}

TEST(MqttPublisher, InvalidPointIsPermanentEvenWhileDisconnected)
{
    MqttPublisher publisher{options_for(closed_port())};
    StatusRecorder recorder;
    publisher.start(recorder.callback());
    auto point = sample_point();
    point.tag = "bad/tag";
    const auto result = publisher.publish(point);
    EXPECT_EQ(result.status, PublishStatus::InvalidPoint);
    EXPECT_FALSE(result.retryable());
    publisher.stop();
}

TEST(MqttPublisher, StopDuringBackoffIsPromptAndSilent)
{
    auto options = options_for(closed_port());
    options.reconnect_initial_delay = 30s;
    options.reconnect_max_delay = 30s;
    MqttPublisher publisher{options};
    StatusRecorder recorder;
    publisher.start(recorder.callback());
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Disconnected, 1, 10s));

    const auto before = Clock::now();
    publisher.stop();
    // The worker wakes at once; destroying the last Paho client stops Paho's shared threads,
    // which polls in 100 ms steps against a receive loop with a 1 s socket wait.
    EXPECT_LT(Clock::now() - before, 2000ms);
    const auto total = recorder.total();
    EXPECT_EQ(total, 2U); // Connecting, Disconnected(refused); nothing from stop()
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(recorder.total(), total);
    EXPECT_EQ(recorder.active(), 0);
}

TEST(MqttPublisher, ThrowingStatusCallbackDoesNotStopTheWorker)
{
    MqttPublisher publisher{options_for(closed_port())};
    StatusRecorder recorder;
    publisher.start(recorder.callback(
        [](const output::PublisherStatus&) { throw std::runtime_error("callback failure"); }));
    EXPECT_TRUE(recorder.wait_for_count(PublisherState::Connecting, 3, 10s));
    publisher.stop();
}

TEST(MqttPublisher, StopFromStatusCallbackIsDetectedWithoutDeadlock)
{
    MqttPublisher publisher{options_for(closed_port())};
    StatusRecorder recorder;
    publisher.start(recorder.callback([&publisher](const output::PublisherStatus& status) {
        if (status.state == PublisherState::Disconnected) {
            publisher.stop(); // precondition violation: must not deadlock
        }
    }));
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Disconnected, 1, 10s));
    publisher.stop(); // a valid stop() still completes the shutdown
    const auto total = recorder.total();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(recorder.total(), total);
    EXPECT_EQ(recorder.count(PublisherState::Disconnected), 1U);
}

} // namespace
} // namespace ieg::mqtt
