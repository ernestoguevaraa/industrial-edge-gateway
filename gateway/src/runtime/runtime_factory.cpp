#include "ieg/mqtt/mqtt_publisher.hpp"
#include "ieg/opcua/open62541_client.hpp"
#include "ieg/runtime/gateway_runtime.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace ieg::runtime {

void validate_for_mvp1(const config::GatewayConfig& config)
{
    std::string errors;
    if (config.mqtt.qos != config::MqttQos::AtLeastOnce) {
        errors += "\n - mqtt.qos must be 1 (at least once) for MVP 1; got " +
                  std::to_string(static_cast<int>(config.mqtt.qos));
    }
    if (!errors.empty()) {
        throw StartupError("configuration is not supported by the MVP 1 runtime:" + errors);
    }
}

RuntimeOptions runtime_options_from(const config::GatewayConfig& config)
{
    return RuntimeOptions{.queue_capacity = config.runtime.queue_capacity,
                          .drain_timeout = config.runtime.shutdown_drain_timeout,
                          .report_interval = RuntimeOptions{}.report_interval};
}

std::unique_ptr<GatewayRuntime> make_gateway_runtime(const config::GatewayConfig& config)
{
    validate_for_mvp1(config);
    if (config.mqtt.client_id.empty()) {
        spdlog::warn("event=mqtt_client_id_fallback message=\"mqtt.client_id is not configured; "
                     "using the development fallback ieg-gateway-<hostname>, which is not stable "
                     "across container recreation\"");
    }
    try {
        auto opcua = std::make_unique<opcua::Open62541Client>(opcua::Open62541ClientOptions{
            .endpoint = config.opcua.endpoint,
            .publishing_interval = config.opcua.subscription_interval});
        auto publisher = std::make_unique<mqtt::MqttPublisher>(
            mqtt::MqttPublisherOptions{.host = config.mqtt.host,
                                       .port = config.mqtt.port,
                                       .topic_prefix = config.mqtt.topic_prefix,
                                       .client_id = config.mqtt.client_id});
        return std::make_unique<GatewayRuntime>(config, std::move(opcua), std::move(publisher),
                                                runtime_options_from(config));
    } catch (const std::invalid_argument& error) {
        throw StartupError(std::string{"invalid gateway configuration: "} + error.what());
    }
}

} // namespace ieg::runtime
