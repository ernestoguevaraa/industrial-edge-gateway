// Integration tests: the real MqttPublisher against a real Mosquitto broker.
// Skipped unless IEG_MOSQUITTO names a mosquitto binary.

#include "ieg/domain/telemetry_json.hpp"
#include "ieg/mqtt/mqtt_publisher.hpp"
#include "integration/mosquitto_process.hpp"
#include "integration/mqtt_test_subscriber.hpp"
#include "integration/simulator_process.hpp"
#include "mqtt/status_recorder.hpp"

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ieg::mqtt {
namespace {

using namespace std::chrono_literals;
using ieg::testing::MosquittoProcess;
using ieg::testing::MqttTestSubscriber;
using ieg::testing::SimulatorProcess;
using output::PublisherState;
using output::PublishStatus;
using output::RetryDisposition;
using testing::StatusRecorder;
using Clock = std::chrono::steady_clock;

domain::TelemetryPoint sample_point(double value)
{
    domain::TelemetryPoint point;
    point.asset = "site01.line01.press01";
    point.tag = "pressure";
    point.value = value;
    point.unit = "bar";
    point.quality = domain::DataQuality::Good;
    point.source_timestamp =
        domain::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 10 / 17}};
    point.protocol = "opcua";
    return point;
}

// Every point in [first, first + count) is published and Delivered.
::testing::AssertionResult publish_delivered(MqttPublisher& publisher, double first, int count)
{
    for (int i = 0; i < count; ++i) {
        const auto result = publisher.publish(sample_point(first + i));
        if (!result.delivered()) {
            return ::testing::AssertionFailure()
                   << "point " << i << ": " << output::to_string(result.status) << " "
                   << result.detail;
        }
    }
    return ::testing::AssertionSuccess();
}

// The subscriber received exactly the payloads of [first, first + count), in order.
::testing::AssertionResult received_in_order(const std::vector<MqttTestSubscriber::Message>& got,
                                             double first, int count)
{
    if (got.size() != static_cast<std::size_t>(count)) {
        return ::testing::AssertionFailure() << "received " << got.size() << " messages";
    }
    for (int i = 0; i < count; ++i) {
        if (got[static_cast<std::size_t>(i)].payload !=
            domain::to_json(sample_point(first + i)).dump()) {
            return ::testing::AssertionFailure() << "payload " << i << " differs";
        }
    }
    return ::testing::AssertionSuccess();
}

// After a deliberate stop(): no Disconnected was reported and no status callback runs later.
::testing::AssertionResult silent_after_stop(const StatusRecorder& recorder)
{
    if (recorder.count(PublisherState::Disconnected) != 0U) {
        return ::testing::AssertionFailure() << "deliberate stop() reported Disconnected";
    }
    const auto total = recorder.total();
    std::this_thread::sleep_for(200ms);
    if (recorder.total() != total || recorder.active() != 0) {
        return ::testing::AssertionFailure() << "status callback ran after stop() returned";
    }
    return ::testing::AssertionSuccess();
}

// The blocked publish() finished within `bound` of `since` with the expected retryable status.
::testing::AssertionResult finished_with(std::future<output::PublishResult>& blocked,
                                         PublishStatus expected, Clock::time_point since,
                                         std::chrono::milliseconds bound)
{
    if (blocked.wait_for(bound + 2s) != std::future_status::ready) {
        return ::testing::AssertionFailure() << "publish() is still blocked";
    }
    const auto result = blocked.get();
    const auto elapsed = Clock::now() - since;
    if (result.status != expected || !result.retryable() || elapsed >= bound) {
        return ::testing::AssertionFailure()
               << "got " << output::to_string(result.status) << " (" << result.detail << ") after "
               << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms";
    }
    return ::testing::AssertionSuccess();
}

// A publish() that is still blocked (no PUBACK) after 300 ms, running on another thread.
std::future<output::PublishResult> start_blocked_publish(MqttPublisher& publisher)
{
    auto blocked = std::async(std::launch::async,
                              [&publisher] { return publisher.publish(sample_point(1.0)); });
    EXPECT_EQ(blocked.wait_for(300ms), std::future_status::timeout) << "publish did not block";
    return blocked;
}

class MosquittoIntegration : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto binary = MosquittoProcess::binary_from_environment();
        if (!binary) {
            GTEST_SKIP() << "IEG_MOSQUITTO is not set; skipping Mosquitto integration";
        }
        broker_ = std::make_unique<MosquittoProcess>(*binary, SimulatorProcess::free_port(),
                                                     "topic write industrial/#\ntopic read #\n");
        broker_->launch();
    }

    [[nodiscard]] MosquittoProcess& broker() const { return *broker_; }

    [[nodiscard]] MqttPublisherOptions options(std::string prefix = "industrial") const
    {
        return MqttPublisherOptions{.host = "127.0.0.1",
                                    .port = broker_->port(),
                                    .topic_prefix = std::move(prefix),
                                    .client_id = "ieg-integration-publisher",
                                    .publish_timeout = 5000ms,
                                    .connect_timeout = 2000ms,
                                    .keep_alive = 10s,
                                    .reconnect_initial_delay = 100ms,
                                    .reconnect_max_delay = 500ms};
    }

    [[nodiscard]] std::string uri() const
    {
        return "tcp://127.0.0.1:" + std::to_string(broker_->port());
    }

    // Starts the publisher and waits for the first Connected status.
    static ::testing::AssertionResult start_connected(MqttPublisher& publisher,
                                                      StatusRecorder& recorder)
    {
        publisher.start(recorder.callback());
        if (!recorder.wait_for_count(PublisherState::Connected, 1, 10s)) {
            return ::testing::AssertionFailure() << "publisher did not connect";
        }
        return ::testing::AssertionSuccess();
    }

private:
    std::unique_ptr<MosquittoProcess> broker_;
};

TEST_F(MosquittoIntegration, DeliversQos1WithExactTopicPayloadAndNoRetain)
{
    MqttTestSubscriber subscriber{uri(), "ieg-integration-subscriber"};
    subscriber.subscribe("industrial/#");

    StatusRecorder recorder;
    MqttPublisher publisher{options()};
    publisher.start(recorder.callback());
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Connected, 1, 10s));

    const auto point = sample_point(180.25);
    const auto result = publisher.publish(point);
    EXPECT_EQ(result.status, PublishStatus::Delivered) << result.detail;
    EXPECT_EQ(result.retry, RetryDisposition::NotApplicable);

    ASSERT_TRUE(subscriber.wait_for_messages(1, 5s));
    const auto message = subscriber.messages().front();
    EXPECT_EQ(message.topic, "industrial/site01/line01/press01/pressure");
    EXPECT_EQ(message.payload, domain::to_json(point).dump());
    EXPECT_EQ(message.qos, 1);
    EXPECT_FALSE(message.retained);

    // retain=false: a subscriber arriving later receives nothing.
    MqttTestSubscriber late{uri(), "ieg-integration-late"};
    late.subscribe("industrial/#");
    EXPECT_FALSE(late.wait_for_messages(1, 500ms));

    publisher.stop();
    const auto statuses = recorder.statuses();
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0].state, PublisherState::Connecting);
    EXPECT_EQ(statuses[1].state, PublisherState::Connected);
}

TEST_F(MosquittoIntegration, AclDenialIsRejectedPermanentlyWithReasonCode)
{
    StatusRecorder recorder;
    MqttPublisher publisher{options("forbidden")};
    publisher.start(recorder.callback());
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Connected, 1, 10s));

    const auto result = publisher.publish(sample_point(1.0));
    EXPECT_EQ(result.status, PublishStatus::Rejected);
    EXPECT_EQ(result.retry, RetryDisposition::Permanent);
    EXPECT_NE(result.detail.find("0x87"), std::string::npos) << result.detail;
    publisher.stop();
}

TEST_F(MosquittoIntegration, ReconnectsAfterBrokerRestartAndReusedPacketIdsStayCorrect)
{
    StatusRecorder recorder;
    MqttPublisher publisher{options()};
    ASSERT_TRUE(start_connected(publisher, recorder));
    EXPECT_TRUE(publish_delivered(publisher, 0.0, 5));

    broker().terminate();
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Disconnected, 1, 10s));
    const auto before = Clock::now();
    const auto offline = publisher.publish(sample_point(99.0));
    EXPECT_LT(Clock::now() - before, 50ms);
    EXPECT_EQ(offline.status, PublishStatus::NotConnected);
    EXPECT_TRUE(offline.retryable());

    broker().launch();
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Connected, 2, 15s));
    // Clean start restarts packet identifiers (Paho tokens) at 1 on the new connection; each
    // publish must still be matched to its own PUBACK.
    MqttTestSubscriber after{uri(), "ieg-integration-after-restart"};
    after.subscribe("industrial/#");
    EXPECT_TRUE(publish_delivered(publisher, 100.0, 5));
    ASSERT_TRUE(after.wait_for_messages(5, 5s));
    EXPECT_TRUE(received_in_order(after.messages(), 100.0, 5));
    publisher.stop();
    EXPECT_FALSE(recorder.overlap_detected());
}

TEST_F(MosquittoIntegration, PublishTimesOutWhenBrokerDoesNotAcknowledge)
{
    StatusRecorder recorder;
    auto opts = options();
    opts.publish_timeout = 500ms;
    MqttPublisher publisher{opts};
    ASSERT_TRUE(start_connected(publisher, recorder));

    broker().pause();
    const auto before = Clock::now();
    const auto result = publisher.publish(sample_point(1.0));
    const auto elapsed = Clock::now() - before;
    broker().resume();

    EXPECT_EQ(result.status, PublishStatus::Timeout);
    EXPECT_TRUE(result.retryable());
    EXPECT_GE(elapsed, 500ms);
    EXPECT_LT(elapsed, 2000ms);
    publisher.stop();
}

TEST_F(MosquittoIntegration, StopInterruptsBlockedPublishPromptlyAndSilently)
{
    StatusRecorder recorder;
    auto opts = options();
    opts.publish_timeout = 30s;
    MqttPublisher publisher{opts};
    ASSERT_TRUE(start_connected(publisher, recorder));

    broker().pause();
    auto blocked = start_blocked_publish(publisher);
    const auto before = Clock::now();
    auto stopping = std::async(std::launch::async, [&] { publisher.stop(); });
    EXPECT_TRUE(finished_with(blocked, PublishStatus::Stopped, before, 500ms));

    ASSERT_EQ(stopping.wait_for(5s), std::future_status::ready); // bounded graceful disconnect
    stopping.get();
    broker().resume();
    EXPECT_TRUE(silent_after_stop(recorder));
}

// Confines the calling thread, and every thread it creates afterwards, to one CPU for its
// lifetime. On one CPU the publish() thread and the publisher's worker compete for the core,
// which reliably exposes ordering races between them.
class SingleCpuScope
{
public:
    SingleCpuScope() : pinned_(pin_to_one_cpu(original_)) {}
    ~SingleCpuScope()
    {
        if (pinned_) {
            ::pthread_setaffinity_np(::pthread_self(), sizeof(original_), &original_);
        }
    }
    SingleCpuScope(const SingleCpuScope&) = delete;
    SingleCpuScope& operator=(const SingleCpuScope&) = delete;
    SingleCpuScope(SingleCpuScope&&) = delete;
    SingleCpuScope& operator=(SingleCpuScope&&) = delete;

private:
    // Saves the current affinity in `original` and restricts the thread to its first CPU.
    static bool pin_to_one_cpu(cpu_set_t& original)
    {
        CPU_ZERO(&original);
        if (::pthread_getaffinity_np(::pthread_self(), sizeof(original), &original) != 0) {
            return false;
        }
        cpu_set_t one{};
        CPU_ZERO(&one);
        for (std::size_t cpu = 0; cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
            if (CPU_ISSET(cpu, &original)) {
                CPU_SET(cpu, &one);
                break;
            }
        }
        return ::pthread_setaffinity_np(::pthread_self(), sizeof(one), &one) == 0;
    }

    cpu_set_t original_{}; // declared before pinned_: initialized first
    bool pinned_{false};
};

// Regression: stop() interrupting an unresolved publish must yield Stopped, even when Paho's
// discard of the pending publication (failure callback "Operation discarded before completion",
// caused by the shutdown's DISCONNECT) is recorded before publish() observes the stop.
TEST_F(MosquittoIntegration, StopDuringBlockedPublishIsStoppedEvenWhenShutdownDiscardWinsTheRace)
{
    constexpr int kIterations = 60;
    // Keeps Paho's shared threads alive, so stopping each publisher is fast.
    MqttTestSubscriber keep_paho_running{uri(), "ieg-integration-keepalive"};
    keep_paho_running.subscribe("unrelated/#");
    const SingleCpuScope single_cpu;

    std::size_t stopped = 0;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        StatusRecorder recorder;
        auto opts = options();
        opts.publish_timeout = 30s;
        MqttPublisher publisher{opts};
        ASSERT_TRUE(start_connected(publisher, recorder)) << "iteration " << iteration;

        broker().pause();
        auto blocked = start_blocked_publish(publisher);
        publisher.stop();
        broker().resume();
        ASSERT_EQ(blocked.wait_for(5s), std::future_status::ready) << "iteration " << iteration;
        const auto result = blocked.get();
        EXPECT_EQ(result.status, PublishStatus::Stopped)
            << "iteration " << iteration << ": " << output::to_string(result.status) << " ("
            << result.detail << ")";
        stopped += result.status == PublishStatus::Stopped ? 1U : 0U;
    }
    EXPECT_EQ(stopped, static_cast<std::size_t>(kIterations));
}

TEST_F(MosquittoIntegration, ConnectionLossDuringPublishIsTimeoutWithUnknownOutcome)
{
    StatusRecorder recorder;
    auto opts = options();
    opts.publish_timeout = 30s;
    MqttPublisher publisher{opts};
    ASSERT_TRUE(start_connected(publisher, recorder));

    broker().pause();
    auto blocked = start_blocked_publish(publisher);
    const auto before = Clock::now();
    broker().kill(); // the kernel closes the connection while the PUBACK is outstanding
    ASSERT_EQ(blocked.wait_for(5s), std::future_status::ready);
    const auto result = blocked.get();
    EXPECT_LT(Clock::now() - before, 2s);
    EXPECT_EQ(result.status, PublishStatus::Timeout);
    EXPECT_TRUE(result.retryable());
    EXPECT_NE(result.detail.find("unknown"), std::string::npos) << result.detail;
    EXPECT_TRUE(recorder.wait_for_count(PublisherState::Disconnected, 1, 5s));
    publisher.stop();
}

TEST_F(MosquittoIntegration, StopWhileConnectedIsSilentAndFinal)
{
    StatusRecorder recorder;
    MqttPublisher publisher{options()};
    publisher.start(recorder.callback());
    ASSERT_TRUE(recorder.wait_for_count(PublisherState::Connected, 1, 10s));

    const auto before = Clock::now();
    publisher.stop();
    EXPECT_LT(Clock::now() - before, 3s); // DISCONNECT (<= 1 s) + Paho thread shutdown (~1 s)
    EXPECT_EQ(recorder.count(PublisherState::Disconnected), 0U);
    const auto total = recorder.total();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(recorder.total(), total);
    EXPECT_EQ(publisher.publish(sample_point(1.0)).status, PublishStatus::Stopped);
}

} // namespace
} // namespace ieg::mqtt
