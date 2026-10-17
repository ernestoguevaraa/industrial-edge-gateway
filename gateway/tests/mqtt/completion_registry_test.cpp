#include "mqtt/completion_registry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <set>
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

} // namespace
} // namespace ieg::mqtt::detail
