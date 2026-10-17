#pragma once

#include <MQTTAsync.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace ieg::testing {

// Minimal MQTT 5 subscriber (Paho MQTTAsync) used to observe what the broker delivers.
class MqttTestSubscriber
{
public:
    struct Message
    {
        std::string topic;
        std::string payload;
        int qos{};
        bool retained{};
    };

    MqttTestSubscriber(const std::string& server_uri, const std::string& client_id)
    {
        MQTTAsync_createOptions create = MQTTAsync_createOptions_initializer5;
        if (MQTTAsync_createWithOptions(&handle_, server_uri.c_str(), client_id.c_str(),
                                        MQTTCLIENT_PERSISTENCE_NONE, nullptr,
                                        &create) != MQTTASYNC_SUCCESS ||
            MQTTAsync_setCallbacks(handle_, this, nullptr, &on_message, nullptr) !=
                MQTTASYNC_SUCCESS) {
            throw std::runtime_error("could not create test subscriber");
        }
    }

    ~MqttTestSubscriber()
    {
        if (handle_ != nullptr) {
            const MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer5;
            MQTTAsync_disconnect(handle_, &options);
            MQTTAsync_destroy(&handle_);
        }
    }

    MqttTestSubscriber(const MqttTestSubscriber&) = delete;
    MqttTestSubscriber& operator=(const MqttTestSubscriber&) = delete;
    MqttTestSubscriber(MqttTestSubscriber&&) = delete;
    MqttTestSubscriber& operator=(MqttTestSubscriber&&) = delete;

    // Connects and subscribes with QoS 1; throws on failure or timeout.
    void subscribe(const std::string& filter)
    {
        MQTTAsync_connectOptions connect = MQTTAsync_connectOptions_initializer5;
        connect.cleanstart = 1;
        connect.onSuccess5 = &on_step_success;
        connect.onFailure5 = &on_step_failure;
        connect.context = this;
        await_step([&] { return MQTTAsync_connect(handle_, &connect); }, "connect");

        MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
        response.onSuccess5 = &on_step_success;
        response.onFailure5 = &on_step_failure;
        response.context = this;
        await_step([&] { return MQTTAsync_subscribe(handle_, filter.c_str(), 1, &response); },
                   "subscribe");
    }

    bool wait_for_messages(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return messages_.size() >= count; });
    }

    [[nodiscard]] std::vector<Message> messages() const
    {
        const std::lock_guard lock(mutex_);
        return messages_;
    }

private:
    enum class Step : std::uint8_t
    {
        Pending,
        Succeeded,
        Failed
    };

    template <typename Start>
    void await_step(Start start, const char* what)
    {
        {
            const std::lock_guard lock(mutex_);
            step_ = Step::Pending;
        }
        if (start() != MQTTASYNC_SUCCESS) {
            throw std::runtime_error(std::string{"test subscriber could not start "} + what);
        }
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds{10},
                               [&] { return step_ != Step::Pending; }) ||
            step_ != Step::Succeeded) {
            throw std::runtime_error(std::string{"test subscriber "} + what + " failed");
        }
    }

    void finish_step(Step result)
    {
        {
            const std::lock_guard lock(mutex_);
            step_ = result;
        }
        changed_.notify_all();
    }

    static void on_step_success(void* context, MQTTAsync_successData5* /*response*/)
    {
        static_cast<MqttTestSubscriber*>(context)->finish_step(Step::Succeeded);
    }

    static void on_step_failure(void* context, MQTTAsync_failureData5* /*response*/)
    {
        static_cast<MqttTestSubscriber*>(context)->finish_step(Step::Failed);
    }

    static int on_message(void* context, char* topic, int topic_length, MQTTAsync_message* message)
    {
        auto& self = *static_cast<MqttTestSubscriber*>(context);
        {
            const std::lock_guard lock(self.mutex_);
            self.messages_.push_back(
                Message{.topic = topic_length > 0
                                     ? std::string{topic, static_cast<std::size_t>(topic_length)}
                                     : std::string{topic},
                        .payload = std::string{static_cast<const char*>(message->payload),
                                               static_cast<std::size_t>(message->payloadlen)},
                        .qos = message->qos,
                        .retained = message->retained != 0});
        }
        self.changed_.notify_all();
        MQTTAsync_freeMessage(&message);
        MQTTAsync_free(topic);
        return 1;
    }

    MQTTAsync handle_{nullptr};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Step step_{Step::Pending};
    std::vector<Message> messages_;
};

} // namespace ieg::testing
