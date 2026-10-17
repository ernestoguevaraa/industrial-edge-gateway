#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ieg::mqtt::testing {

// Thread-safe sink for publisher status callbacks (they arrive on the publisher's worker).
// Also detects overlapping callbacks, which would violate the serialization guarantee.
class StatusRecorder
{
public:
    using Hook = std::function<void(const output::PublisherStatus&)>;

    output::PublisherStatusCallback callback(Hook hook = {})
    {
        hook_ = std::move(hook);
        return [this](const output::PublisherStatus& status) { record(status); };
    }

    bool wait_for_count(output::PublisherState state, std::size_t count,
                        std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return count_unlocked(state) >= count; });
    }

    [[nodiscard]] std::size_t count(output::PublisherState state) const
    {
        const std::lock_guard lock(mutex_);
        return count_unlocked(state);
    }

    [[nodiscard]] std::vector<output::PublisherStatus> statuses() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_;
    }

    [[nodiscard]] std::size_t total() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_.size();
    }

    [[nodiscard]] bool overlap_detected() const
    {
        const std::lock_guard lock(mutex_);
        return overlap_;
    }

    [[nodiscard]] int active() const
    {
        const std::lock_guard lock(mutex_);
        return active_;
    }

private:
    [[nodiscard]] std::size_t count_unlocked(output::PublisherState state) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            statuses_, [state](const output::PublisherStatus& s) { return s.state == state; }));
    }

    void record(const output::PublisherStatus& status)
    {
        {
            const std::lock_guard lock(mutex_);
            overlap_ = overlap_ || active_ > 0;
            ++active_;
            statuses_.push_back(status);
        }
        changed_.notify_all();
        if (hook_) {
            hook_(status);
        }
        const std::lock_guard lock(mutex_);
        --active_;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<output::PublisherStatus> statuses_;
    int active_{0};
    bool overlap_{false};
    Hook hook_;
};

} // namespace ieg::mqtt::testing
