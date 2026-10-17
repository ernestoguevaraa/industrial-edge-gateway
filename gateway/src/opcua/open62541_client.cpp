#include "ieg/opcua/open62541_client.hpp"

#include "common/backoff.hpp"
#include "opcua/ua_conversion.hpp"

#include <open62541.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef UA_ENABLE_SUBSCRIPTIONS
#error "open62541 must be built with UA_ENABLE_SUBSCRIPTIONS"
#endif

namespace ieg::opcua {
namespace {

constexpr UA_UInt32 kIterateTimeoutMs = 100;
constexpr UA_UInt32 kMonitoredItemQueueSize = 10;

std::shared_ptr<spdlog::logger> make_logger()
{
    // Shares the default logger's sinks; the final logging setup is owned by the gateway runtime.
    const auto& base = spdlog::default_logger();
    auto logger =
        std::make_shared<spdlog::logger>("opcua", base->sinks().begin(), base->sinks().end());
    logger->set_level(base->level());
    return logger;
}

spdlog::logger& logger()
{
    static const std::shared_ptr<spdlog::logger> logger = make_logger();
    return *logger;
}

std::string status_name(UA_StatusCode code)
{
    return UA_StatusCode_name(code);
}

// Forwards open62541's own WARNING+ log output into spdlog.
// NOLINTNEXTLINE(cert-dcl50-cpp): signature is dictated by the open62541 logger plugin API.
void forward_open62541_log(void* /*context*/, UA_LogLevel level, UA_LogCategory /*category*/,
                           const char* msg, va_list args)
{
    if (level < UA_LOGLEVEL_WARNING) {
        return;
    }
    std::array<char, 512> buffer{};
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c): printf-style plugin API.
    std::vsnprintf(buffer.data(), buffer.size(), msg, args);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    const auto spd_level = level >= UA_LOGLEVEL_ERROR ? spdlog::level::err : spdlog::level::warn;
    logger().log(spd_level, "event=open62541_log message=\"{}\"", buffer.data());
}

struct ClientDeleter
{
    void operator()(UA_Client* client) const noexcept { UA_Client_delete(client); }
};
using ClientPtr = std::unique_ptr<UA_Client, ClientDeleter>;

} // namespace

class Open62541Client::Impl
{
public:
    explicit Impl(Open62541ClientOptions options) : options_(std::move(options))
    {
        validate_options();
        ua_logger_.log = &forward_open62541_log;
        ua_logger_.context = nullptr;
        ua_logger_.clear = nullptr; // owned by Impl, which outlives every UA_Client
    }

    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start(std::vector<MonitoredItem> items, ClientCallbacks callbacks)
    {
        const std::lock_guard lock(lifecycle_mutex_);
        if (started_) {
            throw std::logic_error("Open62541Client::start() called more than once");
        }
        auto slots = make_slots(std::move(items));
        if (!callbacks.on_data_change || !callbacks.on_status_change) {
            throw std::invalid_argument("Open62541Client::start(): both callbacks are required");
        }
        slots_ = std::move(slots);
        callbacks_ = std::move(callbacks);
        logger().info("event=starting endpoint={} items={}", options_.endpoint, slots_.size());
        // Everything the worker reads is initialised before the thread starts.
        worker_ = std::thread([this, stop = stop_source_.get_token()] { run(stop); });
        started_ = true;
    }

    void stop() noexcept
    {
        if (std::this_thread::get_id() == worker_id_.load()) {
            // Precondition violation: joining ourselves would deadlock. Only request shutdown.
            logger().critical("event=stop_called_from_callback endpoint={} "
                              "message=\"stop() must not be called from an OPC UA callback\"",
                              options_.endpoint);
            stop_source_.request_stop();
            return;
        }
        const std::lock_guard lock(lifecycle_mutex_);
        if (!worker_.joinable()) {
            return;
        }
        stop_source_.request_stop(); // also interrupts the backoff wait
        worker_.join();
        logger().info("event=stopped endpoint={}", options_.endpoint);
    }

private:
    struct ItemSlot
    {
        ItemSlot() { UA_NodeId_init(&node_id); }
        ~ItemSlot() { UA_NodeId_clear(&node_id); }
        ItemSlot(const ItemSlot&) = delete;
        ItemSlot& operator=(const ItemSlot&) = delete;
        ItemSlot(ItemSlot&&) = delete;
        ItemSlot& operator=(ItemSlot&&) = delete;

        Impl* owner{nullptr};
        ItemId id{};
        std::string node_id_text;
        UA_NodeId node_id{};
        // Worker-only: last logged local failure, to avoid logging it on every notification.
        std::string last_conversion_failure;
        bool timestamp_warning_logged{false};
    };

    void validate_options() const
    {
        const auto max_uint32 = std::chrono::milliseconds{std::numeric_limits<UA_UInt32>::max()};
        std::string errors;
        if (!options_.endpoint.starts_with("opc.tcp://")) {
            errors += "\n - endpoint must start with opc.tcp://";
        }
        if (options_.publishing_interval <= std::chrono::milliseconds::zero()) {
            errors += "\n - publishing_interval must be positive";
        }
        if (options_.request_timeout <= std::chrono::milliseconds::zero() ||
            options_.request_timeout > max_uint32) {
            errors += "\n - request_timeout must be positive and fit in 32 bits";
        }
        if (options_.reconnect_initial_delay <= std::chrono::milliseconds::zero()) {
            errors += "\n - reconnect_initial_delay must be positive";
        }
        if (options_.reconnect_max_delay < options_.reconnect_initial_delay) {
            errors += "\n - reconnect_max_delay must be >= reconnect_initial_delay";
        }
        if (!errors.empty()) {
            throw std::invalid_argument("invalid Open62541Client options:" + errors);
        }
    }

    std::vector<std::unique_ptr<ItemSlot>> make_slots(std::vector<MonitoredItem> items)
    {
        std::string errors;
        if (items.empty()) {
            errors += "\n - at least one monitored item is required";
        }
        std::unordered_set<ItemId> ids;
        std::vector<std::unique_ptr<ItemSlot>> slots;
        slots.reserve(items.size());
        for (auto& item : items) {
            if (!ids.insert(item.id).second) {
                errors += "\n - duplicate item id " + std::to_string(item.id);
            }
            auto slot = std::make_unique<ItemSlot>();
            slot->owner = this;
            slot->id = item.id;
            slot->node_id_text = std::move(item.node_id);
            if (slot->node_id_text.empty()) {
                errors += "\n - item " + std::to_string(item.id) + ": empty node_id";
            } else {
                const UA_String text{slot->node_id_text.size(),
                                     reinterpret_cast<UA_Byte*>( // NOLINT(*-reinterpret-cast)
                                         slot->node_id_text.data())};
                if (UA_NodeId_parse(&slot->node_id, text) != UA_STATUSCODE_GOOD) {
                    errors += "\n - item " + std::to_string(item.id) + ": invalid node_id \"" +
                              slot->node_id_text + "\"";
                }
            }
            slots.push_back(std::move(slot));
        }
        if (!errors.empty()) {
            throw std::invalid_argument("invalid OPC UA monitored items:" + errors);
        }
        return slots;
    }

    // ---- Worker thread ------------------------------------------------------------------

    void run(const std::stop_token& stop)
    {
        worker_id_.store(std::this_thread::get_id());
        stop_ = stop;
        while (!stop.stop_requested()) {
            try {
                if (!client_) {
                    if (!connect(stop)) {
                        wait_backoff(stop);
                    }
                    continue;
                }
                const UA_StatusCode result =
                    UA_Client_run_iterate(client_.get(), kIterateTimeoutMs);
                if (result != UA_STATUSCODE_GOOD && !lost_) {
                    mark_lost(status_name(result));
                }
                if (lost_ && !stop.stop_requested()) {
                    handle_loss();
                    wait_backoff(stop);
                }
            } catch (const std::exception& error) {
                logger().error("event=worker_error endpoint={} error=\"{}\"", options_.endpoint,
                               error.what());
                destroy_client();
                wait_backoff(stop);
            }
        }
        destroy_client(); // deliberate stop: no status callback
        worker_id_.store(std::thread::id{});
    }

    bool connect(const std::stop_token& stop)
    {
        emit_status({ConnectionState::Connecting, ""});
        logger().debug("event=connect_attempt endpoint={} failures={}", options_.endpoint,
                       failures_);

        client_ = make_client();
        if (!client_) {
            return fail_attempt("could not allocate open62541 client");
        }
        const UA_StatusCode connected = UA_Client_connect(client_.get(), options_.endpoint.c_str());
        if (stop.stop_requested()) {
            return true; // run() exits and tears down silently
        }
        if (connected != UA_STATUSCODE_GOOD) {
            return fail_attempt(status_name(connected));
        }

        UA_CreateSubscriptionRequest request = UA_CreateSubscriptionRequest_default();
        request.requestedPublishingInterval =
            static_cast<UA_Double>(options_.publishing_interval.count());
        UA_CreateSubscriptionResponse subscription = UA_Client_Subscriptions_create(
            client_.get(), request, this, nullptr, &on_subscription_deleted);
        const UA_StatusCode subscription_result = subscription.responseHeader.serviceResult;
        const UA_UInt32 subscription_id = subscription.subscriptionId;
        const UA_Double revised_publishing_interval = subscription.revisedPublishingInterval;
        UA_CreateSubscriptionResponse_clear(&subscription);
        if (subscription_result != UA_STATUSCODE_GOOD) {
            return fail_attempt("CreateSubscription: " + status_name(subscription_result));
        }

        std::vector<UA_MonitoredItemCreateRequest> item_requests;
        std::vector<void*> contexts;
        item_requests.reserve(slots_.size());
        contexts.reserve(slots_.size());
        for (const auto& slot : slots_) {
            // Shallow copy of the NodeId: the slot keeps ownership.
            UA_MonitoredItemCreateRequest item =
                UA_MonitoredItemCreateRequest_default(slot->node_id);
            item.requestedParameters.samplingInterval =
                static_cast<UA_Double>(options_.publishing_interval.count());
            item.requestedParameters.queueSize = kMonitoredItemQueueSize;
            item.requestedParameters.discardOldest = true;
            item_requests.push_back(item);
            contexts.push_back(slot.get());
        }
        std::vector<UA_Client_DataChangeNotificationCallback> data_callbacks(slots_.size(),
                                                                             &on_data_change);
        std::vector<UA_Client_DeleteMonitoredItemCallback> delete_callbacks(slots_.size(), nullptr);
        UA_CreateMonitoredItemsRequest items_request;
        UA_CreateMonitoredItemsRequest_init(&items_request);
        items_request.subscriptionId = subscription_id;
        items_request.timestampsToReturn = UA_TIMESTAMPSTORETURN_SOURCE;
        items_request.itemsToCreate = item_requests.data();
        items_request.itemsToCreateSize = item_requests.size();

        UA_CreateMonitoredItemsResponse items_response = UA_Client_MonitoredItems_createDataChanges(
            client_.get(), items_request, contexts.data(), data_callbacks.data(),
            delete_callbacks.data());
        const UA_StatusCode items_result = items_response.responseHeader.serviceResult;
        std::vector<std::pair<ItemSlot*, UA_StatusCode>> failed_items;
        if (items_result == UA_STATUSCODE_GOOD) {
            for (std::size_t i = 0; i < items_response.resultsSize && i < slots_.size(); ++i) {
                const UA_StatusCode code =
                    items_response.results[i].statusCode; // NOLINT(*-pointer-arithmetic)
                if (code != UA_STATUSCODE_GOOD) {
                    failed_items.emplace_back(slots_[i].get(), code);
                }
            }
        }
        UA_CreateMonitoredItemsResponse_clear(&items_response);
        if (items_result != UA_STATUSCODE_GOOD) {
            return fail_attempt("CreateMonitoredItems: " + status_name(items_result));
        }

        connected_ = true;
        failures_ = 0;
        logger().info("event=connected endpoint={} publishing_interval_ms={} items={} "
                      "failed_items={}",
                      options_.endpoint, revised_publishing_interval, slots_.size(),
                      failed_items.size());
        emit_status({ConnectionState::Connected, ""});
        for (const auto& [slot, code] : failed_items) {
            logger().warn("event=item_create_failed endpoint={} item_id={} node_id={} status={}",
                          options_.endpoint, slot->id, slot->node_id_text, status_name(code));
            emit_data(DataChange{.id = slot->id,
                                 .value = std::nullopt,
                                 .quality = domain::DataQuality::Bad,
                                 .status_code = code,
                                 .source_timestamp = std::nullopt});
        }
        return true;
    }

    ClientPtr make_client()
    {
        UA_ClientConfig config;
        std::memset(&config, 0, sizeof(config));
        config.logging = &ua_logger_; // set before setDefault so no stdout logger is created
        if (UA_ClientConfig_setDefault(&config) != UA_STATUSCODE_GOOD) {
            UA_ClientConfig_clear(&config);
            return nullptr;
        }
        config.clientContext = this;
        config.timeout = static_cast<UA_UInt32>(options_.request_timeout.count());
        config.securityMode = UA_MESSAGESECURITYMODE_NONE;
        // Recovery is owned by this adapter: once connected, any channel/session loss reported
        // through stateCallback tears this client down and a fresh one is built after backoff.
        // noReconnect must stay false: in v1.4.21 it also forbids the connect-time switch to the
        // discovery URL returned by FindServers, which makes every UA_Client_connect fail with
        // BadNotConnected.
        config.noReconnect = false;
        config.noNewSession = true;
        config.stateCallback = &on_state_change;
        config.subscriptionInactivityCallback = &on_subscription_inactive;
        UA_Client* client = UA_Client_newWithConfig(&config);
        if (client == nullptr) {
            UA_ClientConfig_clear(&config);
        }
        return ClientPtr{client};
    }

    bool fail_attempt(const std::string& reason)
    {
        destroy_client();
        logger().warn("event=connect_failed endpoint={} reason=\"{}\" retry_in_ms={}",
                      options_.endpoint, reason, next_delay().count());
        emit_status({ConnectionState::Disconnected, reason});
        return false;
    }

    void mark_lost(std::string reason)
    {
        if (connected_ && !lost_) {
            lost_ = true;
            loss_reason_ = std::move(reason);
        }
    }

    void handle_loss()
    {
        const std::string reason = loss_reason_;
        destroy_client();
        logger().warn("event=connection_lost endpoint={} reason=\"{}\" retry_in_ms={}",
                      options_.endpoint, reason, next_delay().count());
        emit_status({ConnectionState::Disconnected, reason});
    }

    void destroy_client() noexcept
    {
        if (client_) {
            // Callbacks fired by open62541 while closing are never forwarded.
            tearing_down_ = true;
            UA_Client_disconnect(client_.get());
            client_.reset();
            tearing_down_ = false;
        }
        connected_ = false;
        lost_ = false;
        loss_reason_.clear();
    }

    [[nodiscard]] std::chrono::milliseconds next_delay() const noexcept
    {
        return ieg::detail::backoff_delay(failures_, options_.reconnect_initial_delay,
                                          options_.reconnect_max_delay);
    }

    void wait_backoff(const std::stop_token& stop)
    {
        const auto delay = next_delay();
        ++failures_;
        std::unique_lock lock(wait_mutex_);
        wait_cv_.wait_for(lock, stop, delay, [] { return false; });
    }

    [[nodiscard]] bool may_deliver() const noexcept
    {
        return !tearing_down_ && !stop_.stop_requested();
    }

    void emit_status(const ConnectionStatus& status)
    {
        if (!may_deliver()) {
            return;
        }
        try {
            callbacks_.on_status_change(status);
        } catch (const std::exception& error) {
            logger().error(
                "event=callback_threw endpoint={} callback=on_status_change error=\"{}\"",
                options_.endpoint, error.what());
        } catch (...) {
            logger().error("event=callback_threw endpoint={} callback=on_status_change",
                           options_.endpoint);
        }
    }

    void emit_data(const DataChange& change)
    {
        if (!may_deliver()) {
            return;
        }
        try {
            callbacks_.on_data_change(change);
        } catch (const std::exception& error) {
            logger().error("event=callback_threw endpoint={} callback=on_data_change item_id={} "
                           "error=\"{}\"",
                           options_.endpoint, change.id, error.what());
        } catch (...) {
            logger().error("event=callback_threw endpoint={} callback=on_data_change item_id={}",
                           options_.endpoint, change.id);
        }
    }

    void deliver(ItemSlot& slot, const UA_DataValue& value)
    {
        if (!may_deliver()) {
            return;
        }
        auto converted = detail::to_data_change(slot.id, value);
        if (!converted.conversion_failure.empty()) {
            if (converted.conversion_failure != slot.last_conversion_failure) {
                logger().warn("event=conversion_failed endpoint={} item_id={} node_id={} "
                              "status={} reason=\"{}\"",
                              options_.endpoint, slot.id, slot.node_id_text,
                              status_name(converted.change.status_code),
                              converted.conversion_failure);
                slot.last_conversion_failure = converted.conversion_failure;
            }
        } else if (converted.change.value) {
            slot.last_conversion_failure.clear();
        }
        if (converted.source_timestamp_out_of_range && !slot.timestamp_warning_logged) {
            logger().warn("event=source_timestamp_out_of_range endpoint={} item_id={} node_id={}",
                          options_.endpoint, slot.id, slot.node_id_text);
            slot.timestamp_warning_logged = true;
        }
        emit_data(converted.change);
    }

    // ---- open62541 C callbacks (always invoked on the worker thread) -----------------

    static Impl* from_client(UA_Client* client) noexcept
    {
        return static_cast<Impl*>(UA_Client_getContext(client));
    }

    static void on_data_change(UA_Client* /*client*/, UA_UInt32 /*sub_id*/, void* /*sub_context*/,
                               UA_UInt32 /*mon_id*/, void* mon_context, UA_DataValue* value)
    {
        if (mon_context == nullptr || value == nullptr) {
            return;
        }
        auto* slot = static_cast<ItemSlot*>(mon_context);
        try {
            slot->owner->deliver(*slot, *value);
        } catch (...) {
            // Never let an exception unwind through open62541's C frames.
            logger().error("event=data_change_handling_failed item_id={}", slot->id);
        }
    }

    static void on_state_change(UA_Client* client, UA_SecureChannelState channel,
                                UA_SessionState session, UA_StatusCode connect_status)
    {
        Impl* self = from_client(client);
        if (self == nullptr || !self->connected_ || self->tearing_down_) {
            return;
        }
        try {
            if (connect_status != UA_STATUSCODE_GOOD) {
                self->mark_lost(status_name(connect_status));
            } else if (channel != UA_SECURECHANNELSTATE_OPEN) {
                self->mark_lost("secure channel closed");
            } else if (session != UA_SESSIONSTATE_ACTIVATED) {
                self->mark_lost("session closed");
            }
        } catch (...) {
            self->lost_ = true;
        }
    }

    static void on_subscription_inactive(UA_Client* client, UA_UInt32 /*sub_id*/,
                                         void* /*sub_context*/)
    {
        Impl* self = from_client(client);
        if (self != nullptr && !self->tearing_down_) {
            try {
                self->mark_lost("subscription inactive (no keep-alive)");
            } catch (...) {
                self->lost_ = true;
            }
        }
    }

    static void on_subscription_deleted(UA_Client* client, UA_UInt32 /*sub_id*/,
                                        void* /*sub_context*/)
    {
        Impl* self = from_client(client);
        if (self != nullptr && !self->tearing_down_) {
            try {
                self->mark_lost("subscription deleted");
            } catch (...) {
                self->lost_ = true;
            }
        }
    }

    // ---- State ----------------------------------------------------------------------------

    const Open62541ClientOptions options_;
    UA_Logger ua_logger_{};

    // Lifecycle (owner thread).
    std::mutex lifecycle_mutex_;
    bool started_{false};
    std::atomic<std::thread::id> worker_id_;

    // Written in start() before the worker starts; read-only afterwards (slots are stable).
    std::vector<std::unique_ptr<ItemSlot>> slots_;
    ClientCallbacks callbacks_;

    // Never reassigned, so requesting a stop is race-free from any thread.
    std::stop_source stop_source_;

    // Worker-thread-only state.
    std::stop_token stop_;
    ClientPtr client_;
    bool connected_{false};
    bool lost_{false};
    bool tearing_down_{false};
    std::string loss_reason_;
    unsigned failures_{0};
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;

    // Joined by stop(), which ~Impl() calls before any member is destroyed.
    std::thread worker_;
};

Open62541Client::Open62541Client(Open62541ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{}

Open62541Client::~Open62541Client() = default;

void Open62541Client::start(std::vector<MonitoredItem> items, ClientCallbacks callbacks)
{
    impl_->start(std::move(items), std::move(callbacks));
}

void Open62541Client::stop() noexcept
{
    impl_->stop();
}

} // namespace ieg::opcua
