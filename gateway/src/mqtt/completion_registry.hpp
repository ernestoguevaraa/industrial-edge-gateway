#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ieg::mqtt::detail {

// Registry of in-flight publishes, matched by a process-unique 64-bit id.
//
// Paho reuses MQTTAsync tokens: for QoS 1 the token is the MQTT packet identifier, which cycles
// through 1..65535 and restarts at 1 after every clean-start reconnect. Callback contexts
// therefore carry only a registry id (never a pointer to per-publish state), and ids are never
// reused. A late or duplicate completion, including one from an earlier connection whose token
// matches a newer publish, can only find no entry and is discarded.
//
// Two kinds of completion are distinguished:
//  - complete(): an outcome decided by the broker (PUBACK success or rejection). It is recorded
//    as given and never rewritten, even if the publisher stops afterwards.
//  - complete_unresolved(): the library resolved the publication locally without a broker
//    decision (e.g. Paho discarding it on DISCONNECT or destroy). Recorded as Stopped if the
//    owning publisher was already shutting down at that moment (the discard is a consequence of
//    the shutdown), otherwise as Timeout with an unknown delivery outcome.
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

    // `shutting_down` (optional) is the owning publisher's stop flag; it must outlive the entry
    // (the owner removes the entry before returning from its publish()).
    [[nodiscard]] Id add(const std::atomic<bool>* shutting_down = nullptr)
    {
        const std::lock_guard lock(mutex_);
        const Id id = next_id_++;
        pending_.emplace(id, Entry{.result = std::nullopt, .shutting_down = shutting_down});
        return id;
    }

    // Records the outcome if `id` is still pending and has no outcome yet. Returns false for an
    // unknown (late, abandoned) id or a duplicate completion.
    bool complete(Id id, output::PublishResult result)
    {
        {
            const std::lock_guard lock(mutex_);
            const auto entry = pending_.find(id);
            if (entry == pending_.end() || entry->second.result.has_value()) {
                return false;
            }
            entry->second.result = std::move(result);
        }
        changed_.notify_all();
        return true;
    }

    // Records a local resolution without a broker decision (see class comment). The shutdown
    // check happens under the registry lock at the moment of the callback. Returns false for an
    // unknown (late, abandoned) id or if an outcome was already recorded.
    bool complete_unresolved(Id id, std::string_view reason)
    {
        {
            const std::lock_guard lock(mutex_);
            const auto entry = pending_.find(id);
            if (entry == pending_.end() || entry->second.result.has_value()) {
                return false;
            }
            const auto* shutting_down = entry->second.shutting_down;
            if (shutting_down != nullptr && shutting_down->load()) {
                entry->second.result = output::PublishResult::of(
                    output::PublishStatus::Stopped,
                    "publisher stopped before PUBACK (" + std::string{reason} + ")");
            } else {
                entry->second.result = output::PublishResult::of(
                    output::PublishStatus::Timeout,
                    "publish failed before acknowledgement; delivery outcome unknown: " +
                        std::string{reason});
            }
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
            return entry == pending_.end() || entry->second.result.has_value() || interrupted();
        });
        std::optional<output::PublishResult> result;
        if (const auto entry = pending_.find(id); entry != pending_.end()) {
            result = std::move(entry->second.result);
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
    struct Entry
    {
        std::optional<output::PublishResult> result;
        const std::atomic<bool>* shutting_down{nullptr};
    };

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::unordered_map<Id, Entry> pending_;
    Id next_id_{1};
};

} // namespace ieg::mqtt::detail
