#include "ieg/acquisition/opcua_source.hpp"
#include "ieg/modbus/modbus_tcp_source.hpp"
#include "ieg/mqtt/mqtt_publisher.hpp"
#include "ieg/opcua/open62541_client.hpp"
#include "ieg/runtime/gateway_runtime.hpp"
#include "runtime/telemetry_mapping.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ieg::runtime {
namespace {

using SourceList = std::vector<std::unique_ptr<acquisition::IAcquisitionSource>>;

// One source per configured source; constructing them performs no network activity (clients
// connect on their own worker threads after start()).
SourceList make_sources(const config::GatewayConfig& config)
{
    SourceList sources;
    if (config.opcua) {
        // The OPC UA client is owned by its acquisition source; the runtime only sees the
        // SDK-neutral IAcquisitionSource.
        sources.push_back(std::make_unique<acquisition::OpcUaSource>(
            std::string{detail::kOpcUaSourceId},
            std::make_unique<opcua::Open62541Client>(opcua::Open62541ClientOptions{
                .endpoint = config.opcua->endpoint,
                .publishing_interval = config.opcua->subscription_interval})));
    }
    for (const auto& device : config.modbus.devices) {
        // LibModbusClient is created, used and destroyed on the source's own worker thread.
        sources.push_back(std::make_unique<modbus::ModbusTcpSource>(device));
    }
    return sources;
}

} // namespace

void validate_runtime_config(const config::GatewayConfig& config)
{
    std::string errors;
    if (config.mqtt.qos != config::MqttQos::AtLeastOnce) {
        errors += "\n - mqtt.qos must be 1 (at least once); got " +
                  std::to_string(static_cast<int>(config.mqtt.qos));
    }
    if (!errors.empty()) {
        throw StartupError("configuration is not supported by this runtime:" + errors);
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
    validate_runtime_config(config);
    if (config.mqtt.client_id.empty()) {
        spdlog::warn("event=mqtt_client_id_fallback message=\"mqtt.client_id is not configured; "
                     "using the development fallback ieg-gateway-<hostname>, which is not stable "
                     "across container recreation\"");
    }
    try {
        auto publisher = std::make_unique<mqtt::MqttPublisher>(
            mqtt::MqttPublisherOptions{.host = config.mqtt.host,
                                       .port = config.mqtt.port,
                                       .topic_prefix = config.mqtt.topic_prefix,
                                       .client_id = config.mqtt.client_id});
        return std::make_unique<GatewayRuntime>(config, make_sources(config), std::move(publisher),
                                                runtime_options_from(config));
    } catch (const std::invalid_argument& error) {
        throw StartupError(std::string{"invalid gateway configuration: "} + error.what());
    }
}

} // namespace ieg::runtime
