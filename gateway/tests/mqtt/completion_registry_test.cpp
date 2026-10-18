#include "mqtt/completion_registry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <string>
#include <thread>

namespace ieg::mqtt::detail {
namespace {

using namespace std::chrono_literals;
using output::PublishResult;
using output::PublishStatus;
using Clock = CompletionRegistry::Clock;

TEST(CompletionRegistry, IdsAreUniqueAndNeverReused)
{
    CompletionRegistry registry;
    std::set<CompletionRegistry::Id> ids;
    for (int i = 0; i < 1000; ++i) {
        const auto id = registry.add();
        EXPECT_TRUE(ids.insert(id).second);
        registry.remove(id); // removing never frees the id for reuse
    }
}

TEST(CompletionRegistry, DeliversCompletionToItsOwnPublish)
{
    CompletionRegistry registry;
    const auto id = registry.add();
    EXPECT_TRUE(registry.complete(id, PublishResult::of(PublishStatus::Delivered)));
    const auto result = registry.wait_and_remove(id, Clock::now() + 1s, [] { return false; });
    EXPECT_TRUE(result.has_value() && result->delivered());
    EXPECT_EQ(registry.pending_count(), 0U);
}

TEST(CompletionRegistry, LateCompletionOfAnAbandonedPublishCanNeverSatisfyANewerOne)
{
    // Paho reuses tokens (QoS 1 packet ids restart at 1 after a clean-start reconnect). An
    // outcome for an abandoned publish arrives late, while a newer publish is waiting: it must
    // be discarded, never attributed to the newer publish.
    CompletionRegistry registry;
    const auto old_publish = registry.add();
    EXPECT_FALSE(registry.wait_and_remove(old_publish, Clock::now() + 10ms, [] { return false; }));

    const auto new_publish = registry.add();
    EXPECT_NE(new_publish, old_publish);
    EXPECT_FALSE(registry.complete(old_publish, PublishResult::of(PublishStatus::Delivered)));

    EXPECT_FALSE(registry.wait_and_remove(new_publish, Clock::now() + 20ms, [] { return false; }));
    EXPECT_EQ(registry.pending_count(), 0U);
}

TEST(CompletionRegistry, DuplicateCompletionIsIgnored)
{
    CompletionRegistry registry;
    const auto id = registry.add();
    EXPECT_TRUE(registry.complete(id, PublishResult::of(PublishStatus::Delivered)));
    EXPECT_FALSE(registry.complete(id, PublishResult::of(PublishStatus::Timeout)));
    const auto result = registry.wait_and_remove(id, Clock::now() + 1s, [] { return false; });
    EXPECT_TRUE(result.has_value() && result->delivered());
}

TEST(CompletionRegistry, WakeAllInterruptsAWaiter)
{
    CompletionRegistry registry;
    const auto id = registry.add();
    std::atomic<bool> interrupted{false};
    auto waiter = std::async(std::launch::async, [&] {
        return registry.wait_and_remove(id, Clock::now() + 30s, [&] { return interrupted.load(); });
    });
    std::this_thread::sleep_for(50ms);
    interrupted.store(true);
    registry.wake_all();
    ASSERT_EQ(waiter.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(waiter.get().has_value());
    EXPECT_EQ(registry.pending_count(), 0U);
}

TEST(CompletionRegistry, CompletionFromAnotherThreadWakesTheWaiter)
{
    CompletionRegistry registry;
    const auto id = registry.add();
    auto waiter = std::async(std::launch::async, [&] {
        return registry.wait_and_remove(id, Clock::now() + 30s, [] { return false; });
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(registry.complete(id, PublishResult::of(PublishStatus::Delivered)));
    ASSERT_EQ(waiter.wait_for(5s), std::future_status::ready);
    const auto result = waiter.get();
    EXPECT_TRUE(result.has_value() && result->delivered());
}

TEST(CompletionRegistry, UnresolvedBeforeShutdownIsTimeoutWithUnknownOutcome)
{
    // E.g. Paho discards the publication because the connection dropped on its own.
    CompletionRegistry registry;
    const std::atomic<bool> shutting_down{false};
    const auto id = registry.add(&shutting_down);
    EXPECT_TRUE(registry.complete_unresolved(id, "Operation discarded before completion"));
    const auto result = registry.wait_and_remove(id, Clock::now() + 1s, [] { return false; });
    EXPECT_TRUE(result.has_value() && result->status == PublishStatus::Timeout &&
                result->retryable() &&
                result->detail.find("delivery outcome unknown") != std::string::npos);
}

TEST(CompletionRegistry, UnresolvedAfterShutdownStartedIsStopped)
{
    // Paho discards the publication because our own shutdown disconnected the client.
    CompletionRegistry registry;
    std::atomic<bool> shutting_down{false};
    const auto id = registry.add(&shutting_down);
    shutting_down.store(true);
    EXPECT_TRUE(registry.complete_unresolved(id, "Operation discarded before completion"));
    const auto result = registry.wait_and_remove(id, Clock::now() + 1s, [] { return false; });
    EXPECT_TRUE(result.has_value() && result->status == PublishStatus::Stopped &&
                result->retryable());
}

TEST(CompletionRegistry, UnresolvedWithoutAStopFlagIsTimeout)
{
    CompletionRegistry registry;
    const auto id = registry.add();
    EXPECT_TRUE(registry.complete_unresolved(id, "reason"));
    const auto result = registry.wait_and_remove(id, Clock::now() + 1s, [] { return false; });
    EXPECT_TRUE(result.has_value() && result->status == PublishStatus::Timeout);
}

TEST(CompletionRegistry, BrokerOutcomeRecordedBeforeShutdownIsNeverRewritten)
{
    CompletionRegistry registry;
    std::atomic<bool> shutting_down{false};
    const auto id = registry.add(&shutting_down);
    EXPECT_TRUE(registry.complete(id, PublishResult::of(PublishStatus::Delivered)));
    shutting_down.store(true);
    EXPECT_FALSE(registry.complete_unresolved(id, "Operation discarded before completion"));
    const auto result = registry.wait_and_remove(id, Clock::now() + 1s, [] { return false; });
    EXPECT_TRUE(result.has_value() && result->delivered());
}

// Models the shutdown race: the stopper sets the flag and wakes waiters while a discard caused
// by that shutdown is recorded concurrently, possibly before the waiter re-checks. The waiter
// must never see Timeout: it gets either Stopped (discard recorded first) or no outcome while
// stop is requested (which publish() reports as Stopped).
TEST(CompletionRegistry, ShutdownDiscardRaceNeverYieldsTimeout)
{
    constexpr int kIterations = 2000;
    int timeouts = 0;
    for (int i = 0; i < kIterations; ++i) {
        CompletionRegistry registry;
        std::atomic<bool> shutting_down{false};
        const auto id = registry.add(&shutting_down);
        auto waiter = std::async(std::launch::async, [&] {
            return registry.wait_and_remove(id, Clock::now() + 10s,
                                            [&] { return shutting_down.load(); });
        });
        auto discarder = std::async(std::launch::async, [&] {
            while (!shutting_down.load()) {
                std::this_thread::yield(); // the discard is a consequence of the shutdown
            }
            static_cast<void>(
                registry.complete_unresolved(id, "Operation discarded before completion"));
        });
        shutting_down.store(true);
        registry.wake_all();
        discarder.get();
        ASSERT_EQ(waiter.wait_for(5s), std::future_status::ready);
        const auto result = waiter.get();
        if (result && result->status != PublishStatus::Stopped) {
            ++timeouts;
        }
    }
    EXPECT_EQ(timeouts, 0);
}

} // namespace
} // namespace ieg::mqtt::detail
