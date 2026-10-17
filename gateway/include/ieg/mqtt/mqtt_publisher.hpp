#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

// MQTT 5 implementation of ITelemetryPublisher (Eclipse Paho MQTT C, private dependency).
// This header must never include Paho headers or expose MQTT library types.
namespace ieg::mqtt {

struct MqttPublisherOptions
{
    // Broker host name or IPv4 address; connects to tcp://<host>:<port> (no TLS in MVP 1).
    std::string host;
    std::uint16_t port{1883};
    // One or more topic levels, e.g. "industrial" or "plant/industrial".
    std::string topic_prefix;
    // Empty: development fallback "ieg-gateway-<hostname>". That fallback is NOT a stable
    // identity (e.g. it changes when a container is recreated); configure one for production.
    std::string client_id;
    // Upper bound for one publish() waiting for its PUBACK.
    std::chrono::milliseconds publish_timeout{5000};
    // Connection attempt timeout (rounded up to whole seconds for the MQTT library).
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::seconds keep_alive{10};
    // Reconnect delay after n consecutive failures: min(initial * 2^n, max).
    std::chrono::milliseconds reconnect_initial_delay{500};
    std::chrono::milliseconds reconnect_max_delay{30000};
};

// Publishes each TelemetryPoint as compact JSON to <topic_prefix>/<asset levels>/<tag> with
// QoS 1 and retain=false (both fixed for MVP 1), MQTT 5 clean start and session expiry 0.
//
// One adapter-owned worker thread runs connect/reconnect and is the only thread that invokes
// the status callback. publish() is synchronous: Delivered only after a successful PUBACK.
class MqttPublisher final : public output::ITelemetryPublisher
{
public:
    // Validates the options without any network activity; throws std::invalid_argument.
    explicit MqttPublisher(MqttPublisherOptions options);
    // Calls stop().
    ~MqttPublisher() override;

    MqttPublisher(const MqttPublisher&) = delete;
    MqttPublisher& operator=(const MqttPublisher&) = delete;
    MqttPublisher(MqttPublisher&&) = delete;
    MqttPublisher& operator=(MqttPublisher&&) = delete;

    // Also throws std::runtime_error if the MQTT client cannot be created (resource failure).
    void start(output::PublisherStatusCallback on_status) override;
    output::PublishResult publish(const domain::TelemetryPoint& point) override;
    // A publish() blocked waiting for its PUBACK returns Stopped promptly. stop() itself then
    // sends a bounded graceful DISCONNECT (at most 1 s) and destroys the MQTT client; destroying
    // the process's last Paho client also stops Paho's shared background threads, which
    // typically takes up to about 1 s more.
    //
    // Calling stop() from inside the status callback violates the precondition. It is detected
    // to avoid a self-join deadlock and logged as critical; that call only requests shutdown
    // and returns without the usual guarantee. The owner must still call stop() (or destroy
    // the publisher) from another thread to complete it.
    void stop() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ieg::mqtt
