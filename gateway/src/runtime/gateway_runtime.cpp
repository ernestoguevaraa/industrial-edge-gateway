#include "ieg/runtime/gateway_runtime.hpp"

#include "ieg/core/telemetry_queue.hpp"
#include "runtime/telemetry_mapping.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Threads:
//  - adapter threads (acquisition source worker, publisher worker) invoke the callbacks below.
//    Callbacks only map/timestamp/try_push (data) or update state and notify (status); they
//    never log or block.
//  - consumer thread: the only caller of publish(). Waits while the publisher is not connected.
//  - monitor thread: logs connection transitions and periodic summaries outside the callbacks.

namespace ieg::runtime {
namespace {

using output::PublishResult;
using output::PublishStatus;
using Clock = std::chrono::steady_clock;

// How long one consumer pop waits before re-checking connectivity and shutdown.
constexpr auto kPopWait = std::chrono::milliseconds{200};

std::shared_ptr<spdlog::logger> make_logger()
{
    // Shares the default logger's sinks; main() configures them before the runtime starts.
    const auto& base = spdlog::default_logger();
    auto logger =
        std::make_shared<spdlog::logger>("runtime", base->sinks().begin(), base->sinks().end());
    logger->set_level(base->level());
    return logger;
}

spdlog::logger& logger()
{
    static const std::shared_ptr<spdlog::logger> instance = make_logger();
    return *instance;
}

std::string hex_status(const std::optional<acquisition::NativeStatusCode>& code)
{
    if (!code) {
        return "none";
    }
    std::array<char, 16> buffer{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fixed format, bounded buffer.
    static_cast<void>(std::snprintf(buffer.data(), buffer.size(), "0x%08X", *code));
    return buffer.data();
}

const char* to_string(acquisition::ConnectionState state) noexcept
{
    switch (state) {
    case acquisition::ConnectionState::Connecting:
        return "Connecting";
    case acquisition::ConnectionState::Connected:
        return "Connected";
    case acquisition::ConnectionState::Disconnected:
        return "Disconnected";
    }
    return "Unknown";
}

const char* to_string(output::PublisherState state) noexcept
{
    switch (state) {
    case output::PublisherState::Connecting:
        return "Connecting";
    case output::PublisherState::Connected:
        return "Connected";
    case output::PublisherState::Disconnected:
        return "Disconnected";
    }
    return "Unknown";
}

std::uint64_t load(const std::atomic<std::uint64_t>& counter) noexcept
{
    return counter.load(std::memory_order_relaxed);
}

void bump(std::atomic<std::uint64_t>& counter, std::uint64_t n = 1) noexcept
{
    counter.fetch_add(n, std::memory_order_relaxed);
}

} // namespace

class GatewayRuntime::Impl
{
public:
    Impl(const config::GatewayConfig& config,
         std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources,
         std::unique_ptr<output::ITelemetryPublisher> publisher, RuntimeOptions options)
        : table_(config), options_(options), queue_(options.queue_capacity),
          sources_(order_sources(table_, std::move(sources))), publisher_(std::move(publisher)),
          item_counters_(table_.size()), source_events_(table_.sources().size()),
          broker_(config.mqtt.host + ":" + std::to_string(config.mqtt.port))
    {
        if (!publisher_) {
            throw std::invalid_argument("GatewayRuntime needs a publisher");
        }
        if (options_.drain_timeout <= std::chrono::milliseconds::zero() ||
            options_.report_interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("GatewayRuntime timeouts must be positive");
        }
    }

    ~Impl() { stop(); }

    // Orders `sources` like table.sources(); every configured source exactly once, nothing else.
    static std::vector<std::unique_ptr<acquisition::IAcquisitionSource>>
    order_sources(const detail::ItemTable& table,
                  std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources)
    {
        std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> ordered(
            table.sources().size());
        for (auto& source : sources) {
            if (!source) {
                throw std::invalid_argument("GatewayRuntime received a null acquisition source");
            }
            const auto index = table.source_index(source->id());
            if (!index) {
                throw std::invalid_argument("GatewayRuntime received source '" + source->id() +
                                            "', which the configuration does not define");
            }
            if (ordered[*index]) {
                throw std::invalid_argument("GatewayRuntime received source '" + source->id() +
                                            "' more than once");
            }
            ordered[*index] = std::move(source);
        }
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (!ordered[i]) {
                throw std::invalid_argument("GatewayRuntime is missing source '" +
                                            table.sources()[i].source_id + "'");
            }
        }
        return ordered;
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start()
    {
        const std::lock_guard lifecycle(lifecycle_mutex_);
        if (start_attempted_) {
            throw std::logic_error("GatewayRuntime::start() called more than once");
        }
        start_attempted_ = true;
        try {
            monitor_ = std::thread([this] { monitor(); });
            publisher_->start([this](const output::PublisherStatus& s) { on_publisher_status(s); });
            consumer_ = std::thread([this] { consume(); });
            for (std::size_t i = 0; i < sources_.size(); ++i) {
                // Counted before start(): a source whose start() throws is stopped as well.
                sources_attempted_ = i + 1;
                sources_[i]->start(
                    table_.sources()[i].items,
                    acquisition::Callbacks{
                        .on_update = [this](const acquisition::Update& u) { on_update(u); },
                        .on_status = [this](const acquisition::Status& s) { on_status(s); },
                    });
            }
        } catch (const std::exception& error) {
            logger().critical("event=start_failed error=\"{}\"", error.what());
            shutdown_locked();
            throw;
        }
        logger().info("event=started sources={} mqtt_broker={} items={} queue_capacity_points={} "
                      "drain_timeout_ms={}",
                      sources_.size(), broker_, table_.size(), options_.queue_capacity,
                      options_.drain_timeout.count());
        for (const auto& source : table_.sources()) {
            logger().info("event=source_started source_id={} protocol={} target={} items={}",
                          source.source_id, source.protocol, source.target, source.items.size());
        }
    }

    void stop() noexcept
    {
        try {
            const std::lock_guard lifecycle(lifecycle_mutex_);
            shutdown_locked();
        } catch (...) {
            // Only reachable if locking itself fails; nothing safe is left to do.
            std::terminate();
        }
    }

    [[nodiscard]] RuntimeStats stats() const
    {
        return RuntimeStats{.received = load(received_),
                            .enqueued = load(enqueued_),
                            .queue_dropped = load(queue_dropped_),
                            .no_value = load(no_value_),
                            .unknown_item = load(unknown_item_),
                            .unknown_source = load(unknown_source_),
                            .delivered = load(delivered_),
                            .not_connected = load(not_connected_),
                            .unavailable = load(unavailable_),
                            .timeout = load(timeout_),
                            .rejected = load(rejected_),
                            .invalid_point = load(invalid_point_),
                            .publish_errors = load(publish_errors_),
                            .stopped_in_flight = load(stopped_in_flight_),
                            .shutdown_dropped = load(shutdown_dropped_),
                            .queue = queue_.stats()};
    }

    [[nodiscard]] std::vector<ItemDiagnostics> item_diagnostics() const
    {
        std::vector<ItemDiagnostics> result;
        result.reserve(table_.size());
        for (std::size_t index = 0; index < table_.size(); ++index) {
            const auto& binding = table_.at(index);
            const auto& counters = item_counters_[index];
            result.push_back(
                ItemDiagnostics{.asset = binding.asset,
                                .tag = binding.tag,
                                .source_id = table_.sources()[binding.source].source_id,
                                .protocol = binding.protocol,
                                .locator = binding.locator,
                                .no_value = load(counters.no_value),
                                .last_no_value_status = counters.last_status(),
                                .last_no_value_diagnostic = counters.last_diagnostic()});
        }
        return result;
    }

private:
    struct ItemCounters
    {
        // Bit 32 marks "status present", so one atomic holds the optional native status.
        static constexpr std::uint64_t kHasStatus = std::uint64_t{1} << 32U;

        std::atomic<std::uint64_t> no_value{0};
        std::atomic<std::uint64_t> last_status_word{0};
        std::uint64_t reported{0}; // monitor thread only
        // Per-item lock: only the owning source's (serialized) callback writes, so it is never
        // contended by another source.
        mutable std::mutex diagnostic_mutex;
        std::string diagnostic;

        void record(const std::optional<acquisition::NativeStatusCode>& status,
                    const std::string& reason)
        {
            {
                const std::lock_guard lock(diagnostic_mutex);
                diagnostic = reason;
            }
            last_status_word.store(status ? (kHasStatus | *status) : 0U, std::memory_order_relaxed);
            bump(no_value);
        }

        [[nodiscard]] std::string last_diagnostic() const
        {
            const std::lock_guard lock(diagnostic_mutex);
            return diagnostic;
        }

        [[nodiscard]] std::optional<acquisition::NativeStatusCode> last_status() const noexcept
        {
            const auto word = last_status_word.load(std::memory_order_relaxed);
            if ((word & kHasStatus) == 0U) {
                return std::nullopt;
            }
            return static_cast<acquisition::NativeStatusCode>(word & ~kHasStatus);
        }
    };

    struct StatusEvent
    {
        bool pending{false};
        std::string state;
        std::string detail;
        std::uint64_t transitions{0}; // since the last log line
    };

    struct LoggedEvents
    {
        std::vector<StatusEvent> sources; // indexed like table_.sources()
        StatusEvent publisher;
    };

    // ---- Adapter callbacks: short and non-blocking, no logging ------------------------------

    // Runs concurrently for different sources: the item table is immutable, counters are atomic
    // (or per item), and the queue has its own short critical section.
    void on_update(const acquisition::Update& update)
    {
        const auto edge_timestamp = detail::receipt_time(); // gateway receipt time, before enqueue
        bump(received_);
        const detail::ItemBinding* binding = table_.find(update.source_id, update.item_id);
        if (binding == nullptr) {
            // Never published: an unknown source, or an item its source was not asked for.
            bump(table_.source_index(update.source_id) ? unknown_item_ : unknown_source_);
            return;
        }
        auto point = detail::make_point(*binding, update, edge_timestamp);
        if (!point) {
            item_counters_[table_.index_of(*binding)].record(update.native_status,
                                                             update.diagnostic);
            bump(no_value_);
            return;
        }
        switch (queue_.try_push(std::move(*point))) {
        case core::PushResult::Enqueued:
            bump(enqueued_);
            break;
        case core::PushResult::EnqueuedDroppedOldest:
            bump(enqueued_);
            bump(queue_dropped_);
            break;
        case core::PushResult::Closed:
            break; // shutting down; producers are stopped before close(), so this is rare
        }
    }

    void on_status(const acquisition::Status& status)
    {
        const auto index = table_.source_index(status.source_id);
        if (!index) {
            bump(unknown_source_);
            return;
        }
        {
            const std::lock_guard lock(state_mutex_); // brief: copies two short strings
            record_event_locked(source_events_[*index], to_string(status.state), status.detail);
        }
        state_cv_.notify_all();
    }

    void on_publisher_status(const output::PublisherStatus& status)
    {
        {
            const std::lock_guard lock(state_mutex_);
            publisher_connected_ = status.state == output::PublisherState::Connected;
            record_event_locked(publisher_event_, to_string(status.state), status.detail);
        }
        state_cv_.notify_all();
    }

    static void record_event_locked(StatusEvent& event, const char* state,
                                    const std::string& detail)
    {
        event.pending = true;
        event.state = state;
        event.detail = detail;
        ++event.transitions;
    }

    // ---- Consumer thread ----------------------------------------------------------------------

    void consume() noexcept
    {
        try {
            consume_loop();
        } catch (const std::exception& error) {
            logger().error("event=consumer_failed error=\"{}\"", error.what());
        } catch (...) {
            logger().error("event=consumer_failed");
        }
        consumer_done_.set_value();
    }

    void consume_loop()
    {
        std::uint64_t failure_streak = 0;
        // A point popped just before the publisher disconnected is held here (not published to
        // a disconnected publisher) and published first after the reconnect.
        std::optional<domain::TelemetryPoint> held;
        while (true) {
            {
                // While MQTT is disconnected, do not pop: the bounded queue keeps the most recent
                // points (best effort, in memory only) until the publisher reconnects.
                std::unique_lock lock(state_mutex_);
                state_cv_.wait(lock, [this] { return publisher_connected_ || draining_; });
                if (!publisher_connected_) {
                    // Shutting down while disconnected: the held point was never attempted.
                    bump(shutdown_dropped_, held ? 1U : 0U);
                    return;
                }
            }
            if (!held) {
                // After close() a pop returns at once, so nullopt then means drained.
                const bool closed_before_pop = queue_.closed();
                held = queue_.pop_for(kPopWait);
                if (!held) {
                    if (closed_before_pop) {
                        return;
                    }
                    continue;
                }
                if (!publisher_connected()) {
                    continue; // disconnected while popping: wait with the point held
                }
            }
            const auto point = std::exchange(held, std::nullopt);
            PublishResult result;
            try {
                result = publisher_->publish(*point); // NOLINT(bugprone-unchecked-optional-access)
            } catch (const std::exception& error) {
                bump(publish_errors_);
                logger().error("event=publish_threw error=\"{}\"", error.what());
                continue;
            }
            account(result, failure_streak);
            if (result.status == PublishStatus::Stopped) {
                return; // the publisher was stopped (drain deadline); the rest stays queued
            }
        }
    }

    [[nodiscard]] bool publisher_connected()
    {
        const std::lock_guard lock(state_mutex_);
        return publisher_connected_;
    }

    void account(const PublishResult& result, std::uint64_t& failure_streak)
    {
        switch (result.status) {
        case PublishStatus::Delivered:
            bump(delivered_);
            if (failure_streak > 0) {
                logger().info("event=publish_recovered failed_points={}", failure_streak);
                failure_streak = 0;
            }
            return;
        case PublishStatus::Stopped:
            bump(stopped_in_flight_);
            return;
        case PublishStatus::NotConnected:
            bump(not_connected_);
            break;
        case PublishStatus::Unavailable:
            bump(unavailable_);
            break;
        case PublishStatus::Timeout:
            bump(timeout_);
            break;
        case PublishStatus::Rejected:
            bump(rejected_);
            break;
        case PublishStatus::InvalidPoint:
            bump(invalid_point_);
            break;
        }
        if (failure_streak++ == 0) {
            logger().warn("event=publish_failed status={} retry={} detail=\"{}\" "
                          "note=\"point dropped (no retry in MVP 1); further failures are "
                          "summarized\"",
                          output::to_string(result.status), output::to_string(result.retry),
                          result.detail);
        }
    }

    // ---- Monitor thread: all transition and summary logging --------------------------------

    void monitor() noexcept
    {
        try {
            auto next_report = Clock::now() + options_.report_interval;
            RuntimeStats last_report{};
            std::unique_lock lock(state_mutex_);
            while (true) {
                state_cv_.wait_until(lock, next_report, [this] {
                    return monitor_stop_ || publisher_event_.pending ||
                           std::ranges::any_of(source_events_,
                                               [](const StatusEvent& e) { return e.pending; });
                });
                LoggedEvents events{.sources = {},
                                    .publisher = std::exchange(publisher_event_, StatusEvent{})};
                events.sources.reserve(source_events_.size());
                for (auto& event : source_events_) {
                    events.sources.push_back(std::exchange(event, StatusEvent{}));
                }
                const bool stop = monitor_stop_;
                lock.unlock();
                for (std::size_t i = 0; i < events.sources.size(); ++i) {
                    log_source_status(table_.sources()[i], events.sources[i]);
                }
                log_publisher_status(broker_, events.publisher);
                if (Clock::now() >= next_report || stop) {
                    log_summary(last_report);
                    next_report = Clock::now() + options_.report_interval;
                }
                if (stop) {
                    return;
                }
                lock.lock();
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): logging must never take the process down
        }
    }

    static spdlog::level::level_enum status_level(const std::string& state) noexcept
    {
        if (state == "Disconnected") {
            return spdlog::level::warn;
        }
        return state == "Connected" ? spdlog::level::info : spdlog::level::debug;
    }

    // component is the protocol ("opcua", "modbus_tcp"), as in the MVP 1 "component=opcua".
    static void log_source_status(const detail::SourceSpec& source, const StatusEvent& event)
    {
        if (!event.pending) {
            return;
        }
        logger().log(status_level(event.state),
                     "event=connection_state component={} source_id={} target={} state={} "
                     "detail=\"{}\" transitions={}",
                     source.protocol, source.source_id, source.target, event.state, event.detail,
                     event.transitions);
    }

    static void log_publisher_status(const std::string& broker, const StatusEvent& event)
    {
        if (!event.pending) {
            return;
        }
        logger().log(status_level(event.state),
                     "event=connection_state component=mqtt target={} state={} detail=\"{}\" "
                     "transitions={}",
                     broker, event.state, event.detail, event.transitions);
    }

    void log_summary(RuntimeStats& last)
    {
        const RuntimeStats now = stats();
        const auto failed = [](const RuntimeStats& s) {
            return s.not_connected + s.unavailable + s.timeout + s.rejected + s.invalid_point +
                   s.publish_errors;
        };
        const std::uint64_t lost = (now.queue_dropped - last.queue_dropped) +
                                   (now.no_value - last.no_value) + (failed(now) - failed(last));
        logger().log(lost > 0 ? spdlog::level::warn : spdlog::level::debug,
                     "event=telemetry_summary delivered={} enqueued={} queue_dropped={} "
                     "no_value={} publish_failed={} queue_depth={} queue_high_watermark={}",
                     now.delivered - last.delivered, now.enqueued - last.enqueued,
                     now.queue_dropped - last.queue_dropped, now.no_value - last.no_value,
                     failed(now) - failed(last), now.queue.size, now.queue.high_watermark);
        for (std::size_t index = 0; index < table_.size(); ++index) {
            auto& counters = item_counters_[index];
            const auto total = load(counters.no_value);
            if (total == counters.reported) {
                continue;
            }
            const auto& binding = table_.at(index);
            // OPC UA keeps the MVP 1 key "node_id" for its locator.
            const char* locator_key =
                binding.protocol == detail::kOpcUaProtocol ? "node_id" : "locator";
            logger().warn("event=no_value_updates source_id={} asset={} tag={} {}={} count={} "
                          "last_status={} diagnostic=\"{}\"",
                          table_.sources()[binding.source].source_id, binding.asset, binding.tag,
                          locator_key, binding.locator, total - counters.reported,
                          hex_status(counters.last_status()), counters.last_diagnostic());
            counters.reported = total;
        }
        last = now;
    }

    // ---- Shutdown -------------------------------------------------------------------------------

    void shutdown_locked()
    {
        if (stopped_ || !start_attempted_) {
            return;
        }
        stopped_ = true;
        logger().info("event=stopping");

        // 1. Stop producers: after this no source callback runs. stop() is also required (and
        //    safe) after a start() that threw, since a source may have partially started.
        stop_sources();
        // 2. Close the queue and let the consumer drain what is left.
        queue_.close();
        {
            const std::lock_guard lock(state_mutex_);
            draining_ = true;
        }
        state_cv_.notify_all();
        // 3. Bounded drain: wait for the consumer to finish; past the deadline, stopping the
        //    publisher makes a blocked publish() return Stopped.
        if (consumer_.joinable() &&
            consumer_done_future_.wait_for(options_.drain_timeout) != std::future_status::ready) {
            logger().warn("event=drain_timeout timeout_ms={} queued_points={} "
                          "action=\"stopping publisher to interrupt publish\"",
                          options_.drain_timeout.count(), queue_.stats().size);
            publisher_->stop();
        }
        // 4. Stop the publisher. Always (once a start was attempted): stop() is idempotent and
        //    safe without a successful start(), and it guarantees no status callback runs
        //    afterwards, even if start() threw after starting the publisher's own threads.
        publisher_->stop();
        // 5. Join the consumer; it returns promptly once the publisher is stopped.
        if (consumer_.joinable()) {
            consumer_.join();
        }
        std::uint64_t remaining = 0;
        while (queue_.pop_for(std::chrono::milliseconds::zero())) {
            ++remaining; // never attempted
        }
        bump(shutdown_dropped_, remaining);

        {
            const std::lock_guard lock(state_mutex_);
            monitor_stop_ = true;
        }
        state_cv_.notify_all();
        if (monitor_.joinable()) {
            monitor_.join();
        }
        const auto final_stats = stats();
        logger().info("event=stopped delivered={} received={} queue_dropped={} no_value={} "
                      "stopped_in_flight={} shutdown_dropped={}",
                      final_stats.delivered, final_stats.received, final_stats.queue_dropped,
                      final_stats.no_value, final_stats.stopped_in_flight,
                      final_stats.shutdown_dropped);
    }

    // Stops every source whose start() was attempted. With several sources they are stopped
    // concurrently, so shutdown waits for the slowest source (e.g. one Modbus request timeout)
    // instead of the sum. Each source closes its own client on its own worker thread.
    void stop_sources() noexcept
    {
        const std::span<std::unique_ptr<acquisition::IAcquisitionSource>> attempted{
            sources_.data(), sources_attempted_};
        if (attempted.size() <= 1) {
            for (auto& source : attempted) {
                source->stop();
            }
            return;
        }
        std::vector<std::jthread> stoppers;
        std::size_t delegated = 0;
        try {
            stoppers.reserve(attempted.size());
            for (; delegated < attempted.size(); ++delegated) {
                stoppers.emplace_back([source = attempted[delegated].get()] { source->stop(); });
            }
        } catch (...) { // NOLINT(bugprone-empty-catch): fall back to stopping the rest here
        }
        for (std::size_t i = delegated; i < attempted.size(); ++i) {
            attempted[i]->stop();
        }
        // ~jthread joins every stopper before this returns.
    }

    // ---- State ----------------------------------------------------------------------------------

    const detail::ItemTable table_;
    const RuntimeOptions options_;
    core::TelemetryQueue queue_;
    std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources_; // as table_.sources()
    std::unique_ptr<output::ITelemetryPublisher> publisher_;
    std::vector<ItemCounters> item_counters_; // indexed by ItemTable::index_of()
    std::vector<StatusEvent> source_events_;  // guarded by state_mutex_, as table_.sources()
    const std::string broker_;

    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> queue_dropped_{0};
    std::atomic<std::uint64_t> no_value_{0};
    std::atomic<std::uint64_t> unknown_item_{0};
    std::atomic<std::uint64_t> unknown_source_{0};
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> not_connected_{0};
    std::atomic<std::uint64_t> unavailable_{0};
    std::atomic<std::uint64_t> timeout_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> invalid_point_{0};
    std::atomic<std::uint64_t> publish_errors_{0};
    std::atomic<std::uint64_t> stopped_in_flight_{0};
    std::atomic<std::uint64_t> shutdown_dropped_{0};

    // Guarded by state_mutex_ (shared by the consumer, the monitor and the status callbacks).
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool publisher_connected_{false};
    bool draining_{false};
    bool monitor_stop_{false};
    StatusEvent publisher_event_;

    // Owner thread.
    std::mutex lifecycle_mutex_;
    bool start_attempted_{false};
    bool stopped_{false};
    std::size_t sources_attempted_{0};
    std::promise<void> consumer_done_;
    std::future<void> consumer_done_future_{consumer_done_.get_future()};
    std::thread consumer_;
    std::thread monitor_;
};

namespace {

std::vector<std::unique_ptr<acquisition::IAcquisitionSource>>
single(std::unique_ptr<acquisition::IAcquisitionSource> source)
{
    std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources;
    sources.push_back(std::move(source));
    return sources;
}

} // namespace

GatewayRuntime::GatewayRuntime(
    const config::GatewayConfig& config,
    std::vector<std::unique_ptr<acquisition::IAcquisitionSource>> sources,
    std::unique_ptr<output::ITelemetryPublisher> publisher, RuntimeOptions options)
    : impl_(std::make_unique<Impl>(config, std::move(sources), std::move(publisher), options))
{}

GatewayRuntime::GatewayRuntime(const config::GatewayConfig& config,
                               std::unique_ptr<acquisition::IAcquisitionSource> source,
                               std::unique_ptr<output::ITelemetryPublisher> publisher,
                               RuntimeOptions options)
    : GatewayRuntime(config, single(std::move(source)), std::move(publisher), options)
{}

GatewayRuntime::~GatewayRuntime() = default;

void GatewayRuntime::start()
{
    impl_->start();
}

void GatewayRuntime::stop() noexcept
{
    impl_->stop();
}

RuntimeStats GatewayRuntime::stats() const
{
    return impl_->stats();
}

std::vector<ItemDiagnostics> GatewayRuntime::item_diagnostics() const
{
    return impl_->item_diagnostics();
}

} // namespace ieg::runtime
