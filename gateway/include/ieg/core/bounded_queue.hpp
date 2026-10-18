#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ieg::core {

enum class PushResult : std::uint8_t
{
    Enqueued,
    // Enqueued, but the queue was full: the oldest item was evicted to make room.
    EnqueuedDroppedOldest,
    // The queue is closed; the item was discarded.
    Closed
};

// Consistent snapshot of queue counters, taken under the queue lock.
struct QueueStats
{
    std::size_t capacity{};
    std::size_t size{};
    std::size_t high_watermark{};
    std::uint64_t enqueued{};
    std::uint64_t dequeued{};
    std::uint64_t dropped_overflow{};
    std::uint64_t rejected_closed{};

    friend bool operator==(const QueueStats&, const QueueStats&) = default;
};

// Bounded in-memory FIFO decoupling producers (e.g. OPC UA callbacks) from consumers (e.g. the
// MQTT publishing worker).
//
// This is NOT store-and-forward and NOT an outage-delivery guarantee: items live only in memory,
// and when the queue is full the oldest item is dropped (drop-oldest is the only policy).
// Whether it also bridges short consumer-side outages is the owner's consumer policy (see
// TelemetryQueue).
//
// Storage is a ring buffer preallocated to `capacity` slots; the queue never grows beyond it.
// Thread-safe for any number of producers and consumers, using one mutex and one condition
// variable (no lock-free code).
//
// Shutdown contract (owner's responsibility):
//   1. stop all producers,
//   2. close(),
//   3. let consumers drain and join them,
//   4. destroy the queue.
// No thread may be inside a member function when the queue is destroyed.
template <typename T>
class BoundedQueue
{
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "BoundedQueue elements must be nothrow move constructible");

public:
    // Throws std::invalid_argument if capacity is zero.
    explicit BoundedQueue(std::size_t capacity) : slots_(checked_capacity(capacity)) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;
    ~BoundedQueue() = default;

    // Producer side. Never waits for free capacity or for a consumer: if the queue is full, the
    // oldest item is evicted. It is not lock-free: it briefly acquires the queue mutex and may
    // contend for it with other producers or consumers. The evicted item is destroyed after the
    // mutex is released. After close(), the item is discarded and Closed is returned.
    PushResult try_push(T item)
    {
        // Declared before the lock so it is destroyed after the mutex is released.
        std::optional<T> evicted;
        PushResult result = PushResult::Enqueued;
        {
            const std::lock_guard lock(mutex_);
            if (closed_) {
                ++rejected_closed_;
                return PushResult::Closed;
            }
            if (size_ == slots_.size()) {
                evicted = take_front_locked();
                ++dropped_overflow_;
                result = PushResult::EnqueuedDroppedOldest;
            }
            slots_[(head_ + size_) % slots_.size()].emplace(std::move(item));
            ++size_;
            ++enqueued_;
            high_watermark_ = std::max(high_watermark_, size_);
        }
        not_empty_.notify_one();
        return result;
    }

    // Consumer side. Blocks until an item is available, or until the queue is closed and every
    // item accepted before close() has been taken; then returns nullopt.
    std::optional<T> pop()
    {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return size_ > 0 || closed_; });
        return take_dequeued_locked();
    }

    // Like pop(), but waits at most `timeout`. nullopt means EITHER the timeout expired OR the
    // queue is closed and drained; use closed() if the caller must tell them apart.
    std::optional<T> pop_for(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout, [this] { return size_ > 0 || closed_; })) {
            return std::nullopt;
        }
        return take_dequeued_locked();
    }

    // Rejects further pushes and wakes every waiting consumer. Items already accepted remain
    // available until drained. Idempotent.
    void close() noexcept
    {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] bool closed() const
    {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] QueueStats stats() const
    {
        const std::lock_guard lock(mutex_);
        return QueueStats{.capacity = slots_.size(),
                          .size = size_,
                          .high_watermark = high_watermark_,
                          .enqueued = enqueued_,
                          .dequeued = dequeued_,
                          .dropped_overflow = dropped_overflow_,
                          .rejected_closed = rejected_closed_};
    }

private:
    static std::size_t checked_capacity(std::size_t capacity)
    {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue capacity must be greater than zero");
        }
        return capacity;
    }

    // Requires the lock and size_ > 0. Moves the front item out; only the moved-from shell is
    // destroyed here.
    std::optional<T> take_front_locked() noexcept
    {
        std::optional<T> item = std::move(slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1) % slots_.size();
        --size_;
        return item;
    }

    std::optional<T> take_dequeued_locked() noexcept
    {
        if (size_ == 0) {
            return std::nullopt; // closed and drained
        }
        ++dequeued_;
        return take_front_locked();
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::vector<std::optional<T>> slots_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::size_t high_watermark_{0};
    std::uint64_t enqueued_{0};
    std::uint64_t dequeued_{0};
    std::uint64_t dropped_overflow_{0};
    std::uint64_t rejected_closed_{0};
    bool closed_{false};
};

} // namespace ieg::core
