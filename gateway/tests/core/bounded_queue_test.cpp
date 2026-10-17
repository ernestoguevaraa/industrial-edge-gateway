#include "ieg/core/bounded_queue.hpp"
#include "ieg/core/telemetry_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace ieg::core {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Deadlock guard: a hung thread fails the test fast instead of hanging the test run.
template <typename R>
R get_within(std::future<R>& future, std::chrono::seconds deadline, const char* what)
{
    if (future.wait_for(deadline) != std::future_status::ready) {
        ADD_FAILURE() << what << " did not finish within " << deadline.count() << " s (deadlock?)";
        std::abort(); // a blocked thread cannot be joined; end the run instead of hanging
    }
    return future.get();
}

std::vector<int> drain(BoundedQueue<int>& queue)
{
    std::vector<int> items;
    while (auto item = queue.pop_for(0ms)) {
        items.push_back(*item);
    }
    return items;
}

// ---- Single-threaded semantics -----------------------------------------------------------

TEST(BoundedQueue, RejectsZeroCapacity)
{
    EXPECT_THROW(BoundedQueue<int>{0}, std::invalid_argument);
}

TEST(BoundedQueue, IsFifoAndTracksStats)
{
    BoundedQueue<int> queue{4};
    EXPECT_EQ(queue.try_push(1), PushResult::Enqueued);
    EXPECT_EQ(queue.try_push(2), PushResult::Enqueued);
    EXPECT_EQ(queue.try_push(3), PushResult::Enqueued);
    EXPECT_EQ(queue.pop(), std::optional<int>{1});
    EXPECT_EQ(queue.try_push(4), PushResult::Enqueued);
    EXPECT_EQ(drain(queue), (std::vector<int>{2, 3, 4}));
    EXPECT_EQ(queue.stats(), (QueueStats{.capacity = 4,
                                         .size = 0,
                                         .high_watermark = 3,
                                         .enqueued = 4,
                                         .dequeued = 4,
                                         .dropped_overflow = 0,
                                         .rejected_closed = 0}));
}

TEST(BoundedQueue, WrapsAroundTheRingBuffer)
{
    BoundedQueue<int> queue{3};
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(queue.try_push(i), PushResult::Enqueued);
        EXPECT_EQ(queue.pop(), std::optional<int>{i});
    }
    EXPECT_EQ(queue.stats().high_watermark, 1U);
}

TEST(BoundedQueue, DropsOldestWhenFull)
{
    BoundedQueue<int> queue{3};
    std::vector<PushResult> results;
    for (int i = 1; i <= 5; ++i) {
        results.push_back(queue.try_push(i));
    }
    EXPECT_EQ(results, (std::vector<PushResult>{
                           PushResult::Enqueued, PushResult::Enqueued, PushResult::Enqueued,
                           PushResult::EnqueuedDroppedOldest, PushResult::EnqueuedDroppedOldest}));
    const auto stats = queue.stats();
    EXPECT_EQ(stats.size, 3U);
    EXPECT_EQ(stats.high_watermark, 3U);
    EXPECT_EQ(stats.enqueued, 5U);
    EXPECT_EQ(stats.dropped_overflow, 2U);
    EXPECT_EQ(drain(queue), (std::vector<int>{3, 4, 5}));
}

// Destroying a live Probe calls back into the queue. If an evicted item were destroyed while
// the queue mutex is held, this would self-deadlock (caught by the deadline).
struct Probe
{
    const BoundedQueue<Probe>* queue{nullptr};
    std::atomic<int>* destroyed{nullptr};

    Probe(const BoundedQueue<Probe>* q, std::atomic<int>* counter) : queue(q), destroyed(counter) {}
    Probe(Probe&& other) noexcept
        : queue(std::exchange(other.queue, nullptr)),
          destroyed(std::exchange(other.destroyed, nullptr))
    {}
    Probe& operator=(Probe&& other) noexcept
    {
        queue = std::exchange(other.queue, nullptr);
        destroyed = std::exchange(other.destroyed, nullptr);
        return *this;
    }
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe()
    {
        if (queue != nullptr) {
            static_cast<void>(queue->stats()); // locks the queue mutex
            ++*destroyed;
        }
    }
};

TEST(BoundedQueue, DestroysEvictedItemOutsideTheLock)
{
    BoundedQueue<Probe> queue{1};
    std::atomic<int> destroyed{0};
    auto pushes = std::async(std::launch::async, [&] {
        queue.try_push(Probe{&queue, &destroyed});
        return queue.try_push(Probe{&queue, &destroyed}); // evicts the first probe
    });
    EXPECT_EQ(get_within(pushes, 5s, "push with eviction"), PushResult::EnqueuedDroppedOldest);
    EXPECT_EQ(destroyed.load(), 1);
}

TEST(BoundedQueue, SupportsMoveOnlyElements)
{
    BoundedQueue<std::unique_ptr<int>> queue{2};
    queue.try_push(std::make_unique<int>(7));
    const std::unique_ptr<int> item = queue.pop().value_or(nullptr);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(*item, 7);
}

TEST(BoundedQueue, TelemetryQueueCarriesTelemetryPoints)
{
    TelemetryQueue queue{2};
    domain::TelemetryPoint point;
    point.asset = "site01.line01.oven01";
    point.tag = "temperature";
    point.value = 842.4;
    point.quality = domain::DataQuality::Good;
    point.protocol = "opcua";
    const domain::TelemetryPoint expected = point;
    EXPECT_EQ(queue.try_push(std::move(point)), PushResult::Enqueued);
    EXPECT_EQ(queue.pop(), std::optional<domain::TelemetryPoint>{expected});
}

TEST(BoundedQueue, PopForTimesOutOnEmptyQueue)
{
    BoundedQueue<int> queue{1};
    const auto start = Clock::now();
    EXPECT_FALSE(queue.pop_for(50ms).has_value());
    EXPECT_GE(Clock::now() - start, 50ms);
    EXPECT_FALSE(queue.closed());
}

TEST(BoundedQueue, PopForReturnsAvailableItemImmediately)
{
    BoundedQueue<int> queue{1};
    queue.try_push(5);
    const auto start = Clock::now();
    EXPECT_EQ(queue.pop_for(5s), std::optional<int>{5});
    EXPECT_LT(Clock::now() - start, 1s);
}

TEST(BoundedQueue, CloseRejectsNewItemsAndDrainsAcceptedOnes)
{
    BoundedQueue<int> queue{4};
    queue.try_push(1);
    queue.try_push(2);
    queue.close();
    queue.close(); // idempotent
    EXPECT_TRUE(queue.closed());
    EXPECT_EQ(queue.try_push(3), PushResult::Closed);

    EXPECT_EQ(queue.pop(), std::optional<int>{1});
    EXPECT_EQ(queue.pop_for(5s), std::optional<int>{2});
    EXPECT_FALSE(queue.pop().has_value());

    const auto start = Clock::now();
    EXPECT_FALSE(queue.pop_for(5s).has_value()); // closed and drained: no waiting
    EXPECT_LT(Clock::now() - start, 1s);

    const auto stats = queue.stats();
    EXPECT_EQ(stats.enqueued, 2U);
    EXPECT_EQ(stats.dequeued, 2U);
    EXPECT_EQ(stats.rejected_closed, 1U);
}

// ---- Blocking and wake-up ----------------------------------------------------------------

TEST(BoundedQueue, BlockedConsumerWakesOnPush)
{
    BoundedQueue<int> queue{1};
    auto consumer = std::async(std::launch::async, [&] { return queue.pop(); });
    std::this_thread::sleep_for(50ms); // let it block
    queue.try_push(42);
    EXPECT_EQ(get_within(consumer, 5s, "blocked consumer"), std::optional<int>{42});
}

TEST(BoundedQueue, CloseWakesEveryBlockedConsumer)
{
    BoundedQueue<int> queue{1};
    std::vector<std::future<std::optional<int>>> consumers;
    consumers.reserve(5);
    for (int i = 0; i < 4; ++i) {
        consumers.push_back(std::async(std::launch::async, [&] { return queue.pop(); }));
    }
    consumers.push_back(std::async(std::launch::async, [&] { return queue.pop_for(60s); }));
    std::this_thread::sleep_for(50ms);
    queue.close();
    for (auto& consumer : consumers) {
        EXPECT_FALSE(get_within(consumer, 5s, "consumer after close").has_value());
    }
}

TEST(BoundedQueue, TryPushNeverWaitsForConsumerOrCapacity)
{
    BoundedQueue<int> queue{8};
    auto producer = std::async(std::launch::async, [&] {
        for (int i = 0; i < 10'000; ++i) {
            queue.try_push(i);
        }
    });
    get_within(producer, 5s, "producer on a full queue without consumer");
    const auto stats = queue.stats();
    EXPECT_EQ(stats.size, 8U);
    EXPECT_EQ(stats.dropped_overflow, 10'000U - 8U);
    EXPECT_EQ(drain(queue), (std::vector<int>{9992, 9993, 9994, 9995, 9996, 9997, 9998, 9999}));
}

// ---- Concurrency ---------------------------------------------------------------------------

constexpr int kProducers = 4;
constexpr int kItemsPerProducer = 10'000;
constexpr int kConsumers = 2;
constexpr auto kTotalItems = static_cast<std::size_t>(kProducers) * kItemsPerProducer;

int encode(int producer, int sequence)
{
    return (producer * 1'000'000) + sequence;
}

std::vector<std::future<void>> start_producers(BoundedQueue<int>& queue)
{
    std::vector<std::future<void>> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.push_back(std::async(std::launch::async, [&queue, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                queue.try_push(encode(p, i));
            }
        }));
    }
    return producers;
}

std::vector<std::future<std::vector<int>>> start_consumers(BoundedQueue<int>& queue)
{
    std::vector<std::future<std::vector<int>>> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.push_back(std::async(std::launch::async, [&queue] {
            std::vector<int> received;
            while (auto item = queue.pop()) {
                received.push_back(*item);
            }
            return received;
        }));
    }
    return consumers;
}

// Within one consumer, each producer's items must appear in increasing sequence order.
::testing::AssertionResult per_producer_order_preserved(const std::vector<int>& received)
{
    std::vector<int> last(kProducers, -1);
    for (const int item : received) {
        const auto producer = static_cast<std::size_t>(item / 1'000'000);
        const int sequence = item % 1'000'000;
        if (sequence <= last[producer]) {
            return ::testing::AssertionFailure() << "out of order item " << item;
        }
        last[producer] = sequence;
    }
    return ::testing::AssertionSuccess();
}

// Runs the producers to completion, closes the queue, then joins the consumers (the shutdown
// order contract) and returns every item received, checking per-producer order on the way.
std::vector<int> run_producers_then_close_and_drain(BoundedQueue<int>& queue)
{
    auto consumers = start_consumers(queue);
    auto producers = start_producers(queue);
    for (auto& producer : producers) {
        get_within(producer, 30s, "producer");
    }
    queue.close();

    std::vector<int> all;
    all.reserve(kTotalItems);
    for (auto& consumer : consumers) {
        const auto received = get_within(consumer, 30s, "consumer");
        EXPECT_TRUE(per_producer_order_preserved(received));
        all.insert(all.end(), received.begin(), received.end());
    }
    std::ranges::sort(all);
    return all;
}

std::vector<int> every_produced_item()
{
    std::vector<int> expected;
    expected.reserve(kTotalItems);
    for (int p = 0; p < kProducers; ++p) {
        for (int i = 0; i < kItemsPerProducer; ++i) {
            expected.push_back(encode(p, i));
        }
    }
    return expected;
}

TEST(BoundedQueue, ConcurrentProducersAndConsumersLoseNothingWhenCapacitySuffices)
{
    BoundedQueue<int> queue{kTotalItems};
    EXPECT_EQ(run_producers_then_close_and_drain(queue), every_produced_item()); // exactly once
    EXPECT_EQ(queue.stats().dropped_overflow, 0U);
}

TEST(BoundedQueue, ConcurrentOverflowKeepsCountersConsistent)
{
    BoundedQueue<int> queue{16};
    const auto all = run_producers_then_close_and_drain(queue);
    const auto stats = queue.stats();
    EXPECT_EQ(stats.enqueued, kTotalItems);
    EXPECT_EQ(stats.enqueued, stats.dequeued + stats.dropped_overflow);
    EXPECT_EQ(stats.dequeued, all.size());
    EXPECT_EQ(stats.size, 0U);
    EXPECT_LE(stats.high_watermark, 16U);
    EXPECT_EQ(std::ranges::adjacent_find(all), all.end()); // no duplicates
}

// One shutdown race: two consumers (pop and pop_for), two producers that push until they see
// Closed, and close() after `close_delay`. Returns the final stats and the number received.
std::pair<QueueStats, std::uint64_t> run_shutdown_race(std::chrono::microseconds close_delay)
{
    BoundedQueue<int> queue{8};
    std::atomic<std::uint64_t> received{0};
    std::vector<std::future<void>> threads;
    threads.reserve(4);
    threads.push_back(std::async(std::launch::async, [&] {
        while (queue.pop()) {
            ++received;
        }
    }));
    threads.push_back(std::async(std::launch::async, [&] {
        // pop_for() nullopt is ambiguous (timeout or closed-and-drained). Once closed, a
        // zero-timeout pop_for() returns immediately, so nullopt then means drained.
        while (!queue.closed()) {
            if (queue.pop_for(1ms)) {
                ++received;
            }
        }
        while (queue.pop_for(0ms)) {
            ++received;
        }
    }));
    for (int p = 0; p < 2; ++p) {
        threads.push_back(std::async(std::launch::async, [&queue, p] {
            // Producers that keep pushing until they observe Closed exercise the race even
            // though the runtime contract stops producers before close().
            for (int i = 0; queue.try_push(encode(p, i)) != PushResult::Closed; ++i) {
            }
        }));
    }
    std::this_thread::sleep_for(close_delay);
    queue.close();
    for (auto& thread : threads) {
        get_within(thread, 10s, "thread during shutdown race");
    }
    return {queue.stats(), received.load()};
}

TEST(BoundedQueue, ShutdownRacesNeverDeadlockOrLoseAccounting)
{
    for (int iteration = 0; iteration < 200; ++iteration) {
        SCOPED_TRACE(iteration);
        const auto [stats, received] =
            run_shutdown_race(std::chrono::microseconds{(iteration % 10) * 100});
        EXPECT_EQ(stats.size, 0U);
        EXPECT_EQ(stats.enqueued, stats.dequeued + stats.dropped_overflow);
        EXPECT_EQ(stats.dequeued, received);
        EXPECT_GE(stats.rejected_closed, 2U); // each producer saw Closed at least once
    }
}

} // namespace
} // namespace ieg::core
