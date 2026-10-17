#include "ieg/mqtt/mqtt_publisher.hpp"

#include "common/backoff.hpp"
#include "mqtt/completion_registry.hpp"
#include "mqtt/payload.hpp"
#include "mqtt/reason_codes.hpp"
#include "mqtt/submit_result.hpp"
#include "mqtt/topic.hpp"

#include <MQTTAsync.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// Locking rules (Paho invokes every callback while holding its global mutex):
//  1. Never call a Paho function while holding Impl::mutex_ or the CompletionRegistry lock.
//  2. Paho callbacks only take Impl::mutex_ or the registry lock (never both at once), never
//     call Paho and never invoke user code. User status callbacks run only on the worker thread.
// MQTTAsync_destroy() takes Paho's global mutex, so once it returns no callback for the handle
// is running or can run later.

namespace ieg::mqtt {
namespace {

using output::PublishResult;
using output::PublishStatus;
using Registry = detail::CompletionRegistry;
using Clock = std::chrono::steady_clock;

// MQTT control packet type of PUBACK (OASIS MQTT 5.0, 2.1.2), as reported in failureData5.
constexpr int kPubackPacketType = 4;
// Upper bound for the graceful DISCONNECT during stop().
constexpr auto kDisconnectWait = std::chrono::milliseconds{1000};

std::shared_ptr<spdlog::logger> make_logger()
{
    // Shares the default logger's sinks; the final logging setup is owned by the gateway runtime.
    const auto& base = spdlog::default_logger();
    auto logger =
        std::make_shared<spdlog::logger>("mqtt", base->sinks().begin(), base->sinks().end());
    logger->set_level(base->level());
    return logger;
}

spdlog::logger& logger()
{
    static const std::shared_ptr<spdlog::logger> instance = make_logger();
    return *instance;
}

void forward_paho_trace(enum MQTTASYNC_TRACE_LEVELS level, char* message)
{
    if (message != nullptr) {
        logger().log(level >= MQTTASYNC_TRACE_SEVERE ? spdlog::level::err : spdlog::level::warn,
                     "event=paho_trace message=\"{}\"", message);
    }
}

// Paho's trace hook and level are unsynchronized process-wide globals that Paho's own threads
// read, so they must be set before any Paho thread exists. Doing it during static
// initialization of this translation unit (before main) guarantees that.
bool install_trace_forwarding() noexcept
{
    MQTTAsync_setTraceCallback(&forward_paho_trace);
    MQTTAsync_setTraceLevel(MQTTASYNC_TRACE_ERROR);
    return true;
}

[[maybe_unused]] const bool trace_forwarding_installed = install_trace_forwarding();

// Used from catch (...) in Paho callbacks: exceptions must never unwind through Paho's C frames.
void report_callback_exception(const char* callback) noexcept
{
    try {
        logger().error("event=paho_callback_failed callback={}", callback);
    } catch (...) { // NOLINT(bugprone-empty-catch): logging is best effort here
    }
}

std::string default_client_id()
{
    std::array<char, 256> host{};
    if (::gethostname(host.data(), host.size() - 1) != 0 || host[0] == '\0') {
        return "ieg-gateway";
    }
    return std::string{"ieg-gateway-"} + host.data();
}

// Ids must round-trip through a pointer-sized context without truncation.
static_assert(sizeof(std::uintptr_t) >= sizeof(Registry::Id));

void* to_context(Registry::Id id) noexcept
{
    // The context is an opaque integer id, never a pointer to per-publish state.
    // NOLINTNEXTLINE(*-reinterpret-cast,performance-no-int-to-ptr)
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
}

Registry::Id from_context(void* context) noexcept
{
    // NOLINTNEXTLINE(*-reinterpret-cast)
    return reinterpret_cast<std::uintptr_t>(context);
}

std::string reason_string(const MQTTProperties& properties)
{
    const MQTTProperty* property =
        MQTTProperties_getProperty(&properties, MQTTPROPERTY_CODE_REASON_STRING);
    if (property == nullptr) {
        return {};
    }
    // Paho's C API stores property values in a union; a Reason String uses value.data.
    const MQTTLenString& text = property->value.data; // NOLINT(*-pro-type-union-access)
    if (text.data == nullptr || text.len <= 0) {
        return {};
    }
    return {text.data, static_cast<std::size_t>(text.len)};
}

// Paho returns nullptr for unknown values; never build a std::string from it.
std::string reason_name(enum MQTTReasonCodes code)
{
    const char* name = MQTTReasonCode_toString(code);
    return name != nullptr ? name : "reason code " + std::to_string(static_cast<int>(code));
}

std::string error_text(int code)
{
    const char* text = MQTTAsync_strerror(code);
    return text != nullptr ? text : "error " + std::to_string(code);
}

std::string describe_failure(const MQTTAsync_failureData5* failure)
{
    if (failure == nullptr) {
        return "unknown failure";
    }
    std::string text;
    if (failure->reasonCode != MQTTREASONCODE_SUCCESS) {
        text = reason_name(failure->reasonCode);
    } else {
        text = error_text(failure->code);
    }
    if (failure->message != nullptr) {
        text += std::string{": "} + failure->message;
    }
    return text;
}

// ---- Publish completion callbacks (Paho threads; context = registry id) -----------------

void on_publish_success(void* context, MQTTAsync_successData5* /*response*/)
{
    // Paho calls this only for PUBACK reason codes below 0x80 and does not pass the code, so
    // 0x00 and 0x10 (the only valid ones) cannot be told apart here.
    try {
        static_cast<void>(Registry::instance().complete(
            from_context(context), PublishResult::of(PublishStatus::Delivered)));
    } catch (...) {
        report_callback_exception("on_publish_success");
    }
}

void on_publish_failure(void* context, MQTTAsync_failureData5* response)
{
    try {
        PublishResult result;
        if (response != nullptr && response->packet_type == kPubackPacketType) {
            auto mapping = detail::map_puback_reason(static_cast<int>(response->reasonCode),
                                                     reason_string(response->properties));
            if (mapping.unexpected) {
                logger().warn("event=unexpected_puback_reason detail=\"{}\"",
                              mapping.result.detail);
            }
            result = std::move(mapping.result);
        } else {
            result = PublishResult::of(
                PublishStatus::Timeout,
                "publish failed before acknowledgement; delivery outcome unknown: " +
                    describe_failure(response));
        }
        static_cast<void>(Registry::instance().complete(from_context(context), std::move(result)));
    } catch (...) {
        report_callback_exception("on_publish_failure");
    }
}

} // namespace

class MqttPublisher::Impl
{
public:
    explicit Impl(MqttPublisherOptions options) : options_(std::move(options))
    {
        validate_options();
        client_id_ = options_.client_id.empty() ? default_client_id() : options_.client_id;
        server_uri_ = "tcp://" + options_.host + ":" + std::to_string(options_.port);
    }

    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void start(output::PublisherStatusCallback on_status)
    {
        const std::lock_guard lifecycle(lifecycle_mutex_);
        if (started_) {
            throw std::logic_error("MqttPublisher::start() called more than once");
        }
        if (!on_status) {
            throw std::invalid_argument("MqttPublisher::start(): status callback is required");
        }
        MQTTAsync_createOptions create = MQTTAsync_createOptions_initializer5;
        create.sendWhileDisconnected = 0; // fail fast instead of buffering in the library
        MQTTAsync handle = nullptr;
        int rc = MQTTAsync_createWithOptions(&handle, server_uri_.c_str(), client_id_.c_str(),
                                             MQTTCLIENT_PERSISTENCE_NONE, nullptr, &create);
        if (rc == MQTTASYNC_SUCCESS) {
            rc = MQTTAsync_setConnectionLostCallback(handle, this, &on_connection_lost);
        }
        if (rc == MQTTASYNC_SUCCESS) {
            rc = MQTTAsync_setDisconnected(handle, this, &on_server_disconnect);
        }
        if (rc != MQTTASYNC_SUCCESS) {
            if (handle != nullptr) {
                MQTTAsync_destroy(&handle);
            }
            throw std::runtime_error("cannot create MQTT client: " + error_text(rc));
        }

        handle_ = handle;
        status_callback_ = std::move(on_status);
        {
            const std::lock_guard lock(mutex_);
            accepting_ = true;
        }
        logger().info("event=starting broker={} client_id={}", server_uri_, client_id_);
        // Everything the worker reads is initialised before the thread starts.
        worker_ = std::thread([this] { run(); });
        started_ = true;
    }

    PublishResult publish(const domain::TelemetryPoint& point)
    {
        {
            const std::lock_guard lock(mutex_);
            if (!accepting_) {
                return PublishResult::of(PublishStatus::Stopped, "publisher is not running");
            }
            ++active_publishes_;
        }
        const ActivePublish active{*this};

        auto topic = detail::make_topic(options_.topic_prefix, point.asset, point.tag);
        if (!topic.ok()) {
            return PublishResult::of(PublishStatus::InvalidPoint, topic.error);
        }
        auto payload = detail::encode_payload(point);
        if (!payload.ok()) {
            return PublishResult::of(PublishStatus::InvalidPoint, payload.error);
        }
        if (payload.json.size() > static_cast<std::size_t>(INT_MAX)) {
            return PublishResult::of(PublishStatus::InvalidPoint, "payload too large");
        }

        if (stop_requested_.load()) {
            return PublishResult::of(PublishStatus::Stopped, "publisher is stopping");
        }
        if (!connected_.load()) {
            return PublishResult::of(PublishStatus::NotConnected, "not connected to broker");
        }
        const std::uint64_t epoch = epoch_.load();

        auto& registry = Registry::instance();
        const Registry::Id id = registry.add();
        MQTTAsync_message message = MQTTAsync_message_initializer;
        message.payload = payload.json.data();
        message.payloadlen = static_cast<int>(payload.json.size());
        message.qos = 1;      // fixed for MVP 1
        message.retained = 0; // fixed for MVP 1
        MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
        response.onSuccess5 = &on_publish_success;
        response.onFailure5 = &on_publish_failure;
        response.context = to_context(id);

        // Paho copies topic and payload; nothing it keeps points into this frame.
        const int rc = MQTTAsync_sendMessage(handle_, topic.name.c_str(), &message, &response);
        if (rc != MQTTASYNC_SUCCESS) {
            registry.remove(id);
            return detail::submit_failure_result(rc, error_text(rc));
        }

        const auto deadline = Clock::now() + options_.publish_timeout;
        auto result = registry.wait_and_remove(id, deadline, [this, epoch] {
            return stop_requested_.load() || epoch_.load() != epoch;
        });
        if (result) {
            return std::move(*result);
        }
        if (stop_requested_.load()) {
            return PublishResult::of(PublishStatus::Stopped, "publisher stopped before PUBACK");
        }
        if (epoch_.load() != epoch) {
            return PublishResult::of(PublishStatus::Timeout,
                                     "connection lost before PUBACK; delivery outcome unknown");
        }
        return PublishResult::of(PublishStatus::Timeout,
                                 "no PUBACK within " +
                                     std::to_string(options_.publish_timeout.count()) +
                                     " ms; delivery outcome unknown");
    }

    void stop() noexcept
    {
        if (std::this_thread::get_id() == worker_id_.load()) {
            // Precondition violation (called from the status callback): joining ourselves would
            // deadlock, so only request shutdown.
            logger().critical("event=stop_called_from_callback broker={} "
                              "message=\"stop() must not be called from a status callback\"",
                              server_uri_);
            request_stop();
            return;
        }
        const std::lock_guard lifecycle(lifecycle_mutex_);
        if (!started_ || stopped_) {
            return;
        }
        request_stop(); // wakes the worker and any publish() blocked waiting for a PUBACK
        {
            std::unique_lock lock(mutex_);
            idle_cv_.wait(lock, [this] { return active_publishes_ == 0; });
        }
        if (worker_.joinable()) {
            worker_.join(); // the worker disconnects gracefully; no status is reported
        }
        if (handle_ != nullptr) {
            MQTTAsync_destroy(&handle_); // after this no Paho callback can touch this Impl
        }
        stopped_ = true;
        logger().info("event=stopped broker={}", server_uri_);
    }

private:
    enum class ConnectState : std::uint8_t
    {
        Idle,
        Pending,
        Succeeded,
        Failed
    };

    // Counts a publish() as active so stop() never destroys the handle underneath it.
    class ActivePublish
    {
    public:
        explicit ActivePublish(Impl& owner) : owner_(owner) {}
        ~ActivePublish()
        {
            {
                const std::lock_guard lock(owner_.mutex_);
                --owner_.active_publishes_;
            }
            owner_.idle_cv_.notify_all();
        }
        ActivePublish(const ActivePublish&) = delete;
        ActivePublish& operator=(const ActivePublish&) = delete;
        ActivePublish(ActivePublish&&) = delete;
        ActivePublish& operator=(ActivePublish&&) = delete;

    private:
        Impl& owner_;
    };

    void validate_options()
    {
        std::string errors;
        if (options_.host.empty() || options_.host.find_first_of(" \t\r\n/") != std::string::npos) {
            errors += "\n - host must be a non-empty host name or IPv4 address";
        }
        if (options_.port == 0) {
            errors += "\n - port must not be 0";
        }
        if (const auto error = detail::topic_prefix_error(options_.topic_prefix); !error.empty()) {
            errors += "\n - " + error;
        }
        if (!options_.client_id.empty() && (options_.client_id.size() > detail::kMaxTopicBytes ||
                                            !detail::is_valid_mqtt_utf8(options_.client_id))) {
            errors += "\n - client_id must be valid UTF-8 of at most 65535 bytes";
        }
        if (options_.publish_timeout <= std::chrono::milliseconds::zero()) {
            errors += "\n - publish_timeout must be positive";
        }
        if (options_.connect_timeout <= std::chrono::milliseconds::zero() ||
            options_.connect_timeout > std::chrono::hours{24}) {
            errors += "\n - connect_timeout must be positive and at most 24 h";
        }
        if (options_.keep_alive <= std::chrono::seconds::zero() ||
            options_.keep_alive > std::chrono::seconds{65535}) {
            errors += "\n - keep_alive must be within 1..65535 s";
        }
        if (options_.reconnect_initial_delay <= std::chrono::milliseconds::zero()) {
            errors += "\n - reconnect_initial_delay must be positive";
        }
        if (options_.reconnect_max_delay < options_.reconnect_initial_delay) {
            errors += "\n - reconnect_max_delay must be >= reconnect_initial_delay";
        }
        if (!errors.empty()) {
            throw std::invalid_argument("invalid MqttPublisher options:" + errors);
        }
    }

    // ---- Worker thread: connection state machine and status callbacks ------------------

    void run()
    {
        worker_id_.store(std::this_thread::get_id());
        unsigned failures = 0;
        while (!stop_requested_.load()) {
            emit({output::PublisherState::Connecting, ""});
            std::string reason;
            if (connect_once(reason)) {
                failures = 0;
                logger().info("event=connected broker={} client_id={}", server_uri_, client_id_);
                emit({output::PublisherState::Connected, ""});
                reason = wait_for_loss_or_stop();
                if (stop_requested_.load()) {
                    break;
                }
                logger().warn("event=connection_lost broker={} reason=\"{}\" retry_in_ms={}",
                              server_uri_, reason, delay_for(failures).count());
            } else {
                if (stop_requested_.load()) {
                    break;
                }
                logger().warn("event=connect_failed broker={} reason=\"{}\" retry_in_ms={}",
                              server_uri_, reason, delay_for(failures).count());
            }
            emit({output::PublisherState::Disconnected, reason});
            wait_backoff(delay_for(failures));
            ++failures;
        }
        disconnect_gracefully(); // deliberate stop: no status callback
        worker_id_.store(std::thread::id{});
    }

    bool connect_once(std::string& failure)
    {
        {
            const std::lock_guard lock(mutex_);
            connect_state_ = ConnectState::Pending;
            connect_failure_.clear();
        }
        MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer5;
        options.keepAliveInterval = static_cast<int>(options_.keep_alive.count());
        options.cleanstart = 1; // with no Session Expiry Interval property: expiry 0
        options.connectTimeout = connect_timeout_seconds();
        options.automaticReconnect = 0; // reconnect is owned by this worker
        options.onSuccess5 = &on_connect_success;
        options.onFailure5 = &on_connect_failure;
        options.context = this;
        const int rc = MQTTAsync_connect(handle_, &options);
        if (rc != MQTTASYNC_SUCCESS) {
            failure = "connect not started: " + error_text(rc);
            return false;
        }

        std::unique_lock lock(mutex_);
        // Paho reports every attempt through onSuccess5/onFailure5 within connectTimeout; the
        // extra margin only guards against a library defect.
        const auto bound =
            std::chrono::seconds{connect_timeout_seconds()} + std::chrono::seconds{5};
        const bool decided = worker_cv_.wait_for(lock, bound, [this] {
            return connect_state_ != ConnectState::Pending || stop_requested_.load();
        });
        if (stop_requested_.load()) {
            return false;
        }
        if (!decided) {
            connect_state_ = ConnectState::Idle; // ignore any late outcome of this attempt
            failure = "no connect outcome reported by the MQTT library";
            return false;
        }
        if (connect_state_ == ConnectState::Failed) {
            failure = connect_failure_;
            return false;
        }
        return true;
    }

    std::string wait_for_loss_or_stop()
    {
        std::unique_lock lock(mutex_);
        worker_cv_.wait(lock, [this] { return lost_ || stop_requested_.load(); });
        lost_ = false;
        return loss_reason_;
    }

    void wait_backoff(std::chrono::milliseconds delay)
    {
        std::unique_lock lock(mutex_);
        worker_cv_.wait_for(lock, delay, [this] { return stop_requested_.load(); });
    }

    void disconnect_gracefully()
    {
        if (!connected_.exchange(false)) {
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            disconnect_done_ = false;
        }
        MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer5;
        options.timeout = 0; // do not wait for in-flight publications
        options.onSuccess5 = &on_disconnect_success;
        options.onFailure5 = &on_disconnect_failure;
        options.context = this;
        if (MQTTAsync_disconnect(handle_, &options) == MQTTASYNC_SUCCESS) {
            std::unique_lock lock(mutex_);
            worker_cv_.wait_for(lock, kDisconnectWait, [this] { return disconnect_done_; });
        }
    }

    [[nodiscard]] std::chrono::milliseconds delay_for(unsigned failures) const noexcept
    {
        return ieg::detail::backoff_delay(failures, options_.reconnect_initial_delay,
                                          options_.reconnect_max_delay);
    }

    [[nodiscard]] int connect_timeout_seconds() const noexcept
    {
        const auto seconds = std::chrono::ceil<std::chrono::seconds>(options_.connect_timeout);
        return static_cast<int>(seconds.count());
    }

    void request_stop() noexcept
    {
        {
            const std::lock_guard lock(mutex_);
            accepting_ = false;
            stop_requested_.store(true);
        }
        worker_cv_.notify_all();
        Registry::instance().wake_all();
    }

    void emit(const output::PublisherStatus& status)
    {
        if (stop_requested_.load()) {
            return;
        }
        try {
            status_callback_(status);
        } catch (const std::exception& error) {
            logger().error("event=callback_threw broker={} error=\"{}\"", server_uri_,
                           error.what());
        } catch (...) {
            logger().error("event=callback_threw broker={}", server_uri_);
        }
    }

    // ---- Connection callbacks (Paho threads, Paho mutex held; context = Impl*) ---------
    // The Impl outlives the handle: stop() destroys the handle before the Impl goes away.

    static Impl& self(void* context) noexcept { return *static_cast<Impl*>(context); }

    static void on_connect_success(void* context, MQTTAsync_successData5* /*response*/)
    {
        Impl& impl = self(context);
        {
            const std::lock_guard lock(impl.mutex_);
            if (impl.connect_state_ != ConnectState::Pending) {
                return; // stale outcome of an abandoned attempt
            }
            impl.connect_state_ = ConnectState::Succeeded;
            impl.lost_ = false;
            impl.connected_.store(true);
            impl.epoch_.fetch_add(1);
        }
        impl.worker_cv_.notify_all();
        Registry::instance().wake_all();
    }

    static void on_connect_failure(void* context, MQTTAsync_failureData5* response)
    {
        Impl& impl = self(context);
        try {
            std::string reason = describe_failure(response);
            {
                const std::lock_guard lock(impl.mutex_);
                if (impl.connect_state_ != ConnectState::Pending) {
                    return;
                }
                impl.connect_state_ = ConnectState::Failed;
                impl.connect_failure_ = std::move(reason);
            }
            impl.worker_cv_.notify_all();
        } catch (...) {
            report_callback_exception("on_connect_failure");
        }
    }

    void mark_lost(std::string reason) noexcept
    {
        {
            const std::lock_guard lock(mutex_);
            if (!connected_.load()) {
                return;
            }
            connected_.store(false);
            lost_ = true;
            loss_reason_ = std::move(reason);
            epoch_.fetch_add(1);
        }
        worker_cv_.notify_all();
        Registry::instance().wake_all(); // publishes waiting for a PUBACK return Timeout
    }

    static void on_connection_lost(void* context, char* cause)
    {
        try {
            self(context).mark_lost(cause != nullptr ? std::string{cause} : "connection lost");
        } catch (...) {
            report_callback_exception("on_connection_lost");
        }
    }

    static void on_server_disconnect(void* context, MQTTProperties* /*properties*/,
                                     enum MQTTReasonCodes reason_code)
    {
        try {
            self(context).mark_lost("server sent DISCONNECT: " + reason_name(reason_code));
        } catch (...) {
            report_callback_exception("on_server_disconnect");
        }
    }

    void disconnect_finished() noexcept
    {
        {
            const std::lock_guard lock(mutex_);
            disconnect_done_ = true;
        }
        worker_cv_.notify_all();
    }

    static void on_disconnect_success(void* context, MQTTAsync_successData5* /*response*/)
    {
        self(context).disconnect_finished();
    }

    static void on_disconnect_failure(void* context, MQTTAsync_failureData5* /*response*/)
    {
        self(context).disconnect_finished();
    }

    // ---- State ----------------------------------------------------------------------------

    const MqttPublisherOptions options_;
    std::string client_id_;
    std::string server_uri_;

    // Owner thread (start/stop).
    std::mutex lifecycle_mutex_;
    bool started_{false};
    bool stopped_{false};
    MQTTAsync handle_{nullptr};
    output::PublisherStatusCallback status_callback_;
    std::thread worker_;
    std::atomic<std::thread::id> worker_id_;

    // Shared state, guarded by mutex_ (atomics are also read without it).
    std::mutex mutex_;
    std::condition_variable worker_cv_;
    std::condition_variable idle_cv_;
    bool accepting_{false};
    int active_publishes_{0};
    ConnectState connect_state_{ConnectState::Idle};
    std::string connect_failure_;
    bool lost_{false};
    std::string loss_reason_;
    bool disconnect_done_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> connected_{false};
    // Incremented on every connect and every loss; a publish waiting for its PUBACK detects a
    // connection change through it.
    std::atomic<std::uint64_t> epoch_{0};
};

MqttPublisher::MqttPublisher(MqttPublisherOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{}

MqttPublisher::~MqttPublisher() = default;

void MqttPublisher::start(output::PublisherStatusCallback on_status)
{
    impl_->start(std::move(on_status));
}

PublishResult MqttPublisher::publish(const domain::TelemetryPoint& point)
{
    return impl_->publish(point);
}

void MqttPublisher::stop() noexcept
{
    impl_->stop();
}

} // namespace ieg::mqtt
