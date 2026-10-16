#pragma once

#include "ieg/opcua/opcua_client.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ieg::opcua::testing {

// Thread-safe sink for IOpcUaClient callbacks (they arrive on the adapter's worker thread).
// Also detects overlapping callbacks, which would violate the serialization guarantee.
class CallbackRecorder
{
public:
    using StatusHook = std::function<void(const ConnectionStatus&)>;

    ClientCallbacks callbacks(StatusHook hook = {})
    {
        hook_ = std::move(hook);
        return ClientCallbacks{
            .on_data_change = [this](const DataChange& change) { record_data(change); },
            .on_status_change = [this](const ConnectionStatus& s) { record_status(s); },
        };
    }

    template <typename Predicate>
    bool wait_until(Predicate predicate, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(*this); });
    }

    // *_unlocked accessors: only inside wait_until predicates, where the mutex is held.
    [[nodiscard]] const std::vector<ConnectionStatus>& statuses_unlocked() const
    {
        return statuses_;
    }
    [[nodiscard]] const std::vector<DataChange>& data_unlocked() const { return data_; }

    [[nodiscard]] std::size_t count_state_unlocked(ConnectionState state) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            statuses_, [state](const ConnectionStatus& s) { return s.state == state; }));
    }

    // Locked variant for use outside wait_until predicates (the worker may still be running).
    [[nodiscard]] std::size_t count_state(ConnectionState state) const
    {
        const std::lock_guard lock(mutex_);
        return count_state_unlocked(state);
    }

    [[nodiscard]] std::vector<ConnectionStatus> statuses() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_;
    }
    [[nodiscard]] std::vector<DataChange> data() const
    {
        const std::lock_guard lock(mutex_);
        return data_;
    }
    [[nodiscard]] std::size_t total_calls() const
    {
        const std::lock_guard lock(mutex_);
        return statuses_.size() + data_.size();
    }
    [[nodiscard]] bool overlap_detected() const
    {
        const std::lock_guard lock(mutex_);
        return overlap_;
    }
    [[nodiscard]] int active_callbacks() const
    {
        const std::lock_guard lock(mutex_);
        return active_;
    }

private:
    class ActiveScope
    {
    public:
        explicit ActiveScope(CallbackRecorder& owner) : owner_(owner)
        {
            const std::lock_guard lock(owner_.mutex_);
            if (++owner_.active_ > 1) {
                owner_.overlap_ = true;
            }
        }
        ~ActiveScope()
        {
            const std::lock_guard lock(owner_.mutex_);
            --owner_.active_;
        }
        ActiveScope(const ActiveScope&) = delete;
        ActiveScope& operator=(const ActiveScope&) = delete;
        ActiveScope(ActiveScope&&) = delete;
        ActiveScope& operator=(ActiveScope&&) = delete;

    private:
        CallbackRecorder& owner_;
    };

    void record_data(const DataChange& change)
    {
        const ActiveScope scope(*this);
        {
            const std::lock_guard lock(mutex_);
            data_.push_back(change);
        }
        changed_.notify_all();
    }

    void record_status(const ConnectionStatus& status)
    {
        const ActiveScope scope(*this);
        {
            const std::lock_guard lock(mutex_);
            statuses_.push_back(status);
        }
        changed_.notify_all();
        if (hook_) {
            hook_(status);
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<ConnectionStatus> statuses_;
    std::vector<DataChange> data_;
    int active_{0};
    bool overlap_{false};
    StatusHook hook_;
};

} // namespace ieg::opcua::testing
