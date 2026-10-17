#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace ieg::mqtt::detail {

// Registry of in-flight publishes, matched by a process-unique 64-bit id.
//
// Paho reuses MQTTAsync tokens: for QoS 1 the token is the MQTT packet identifier, which cycles
// through 1..65535 and restarts at 1 after every clean-start reconnect. Callback contexts
// therefore carry only a registry id (never a pointer to per-publish state), and ids are never
// reused. A late or duplicate completion, including one from an earlier connection whose token
// matches a newer publish, can only find no entry and is discarded.
class CompletionRegistry
{
public:
    using Id = std::uint64_t;
    using Clock = std::chrono::steady_clock;

    CompletionRegistry() = default;
    CompletionRegistry(const CompletionRegistry&) = delete;
    CompletionRegistry& operator=(const CompletionRegistry&) = delete;
    CompletionRegistry(CompletionRegistry&&) = delete;
    CompletionRegistry& operator=(CompletionRegistry&&) = delete;
    ~CompletionRegistry() = default;

    // Shared by all publishers; lives until process exit.
    static CompletionRegistry& instance()
    {
        static CompletionRegistry registry;
        return registry;
    }

    [[nodiscard]] Id add()
    {
        const std::lock_guard lock(mutex_);
        const Id id = next_id_++;
        pending_.emplace(id, std::nullopt);
        return id;
    }

    // Records the outcome if `id` is still pending and has no outcome yet. Returns false for an
    // unknown (late, abandoned) id or a duplicate completion.
    bool complete(Id id, output::PublishResult result)
    {
        {
            const std::lock_guard lock(mutex_);
            const auto entry = pending_.find(id);
            if (entry == pending_.end() || entry->second.has_value()) {
                return false;
            }
            entry->second = std::move(result);
        }
        changed_.notify_all();
        return true;
    }

    // Waits until `id` has an outcome, `interrupted()` returns true or `deadline` passes. The
    // entry is removed in every case; returns the outcome if one arrived. `interrupted` is
    // evaluated under the registry lock and must only read atomics.
    template <typename Interrupted>
    std::optional<output::PublishResult> wait_and_remove(Id id, Clock::time_point deadline,
                                                         Interrupted interrupted)
    {
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, deadline, [&] {
            const auto entry = pending_.find(id);
            return entry == pending_.end() || entry->second.has_value() || interrupted();
        });
        std::optional<output::PublishResult> result;
        if (const auto entry = pending_.find(id); entry != pending_.end()) {
            result = std::move(entry->second);
            pending_.erase(entry);
        }
        return result;
    }

    void remove(Id id)
    {
        const std::lock_guard lock(mutex_);
        pending_.erase(id);
    }

    // Wakes all waiters so they re-evaluate their interruption predicate. Callers update the
    // atomics read by that predicate first; taking the lock here prevents lost wake-ups.
    void wake_all()
    {
        {
            const std::lock_guard lock(mutex_);
        }
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t pending_count() const
    {
        const std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<Id, std::optional<output::PublishResult>> pending_;
    Id next_id_{1};
};

} // namespace ieg::mqtt::detail
