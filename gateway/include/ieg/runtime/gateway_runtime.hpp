#pragma once

#include "ieg/config/gateway_config.hpp"
#include "ieg/core/bounded_queue.hpp"
#include "ieg/opcua/opcua_client.hpp"
#include "ieg/output/telemetry_publisher.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Gateway runtime: wires an OPC UA client (producer), the bounded telemetry queue and a
// telemetry publisher (single consumer thread) together, with bounded shutdown.
namespace ieg::runtime {

struct RuntimeOptions
{
    // In telemetry points. How long it bridges an MQTT outage depends on the telemetry rate.
    std::size_t queue_capacity{10'000};
    // Bound for publishing the points still queued at shutdown.
    std::chrono::milliseconds drain_timeout{3000};
    // Period of the telemetry summary log.
    std::chrono::milliseconds report_interval{10'000};
};

// Counters since start. `queue` is a snapshot of the telemetry queue.
struct RuntimeStats
{
    std::uint64_t received{};      // DataChange callbacks
    std::uint64_t enqueued{};      // points accepted by the queue
    std::uint64_t queue_dropped{}; // oldest points evicted because the queue was full
    std::uint64_t no_value{};      // DataChanges without a value (no TelemetryPoint created)
    std::uint64_t unknown_item{};  // DataChanges for an ItemId the runtime never requested
    std::uint64_t delivered{};
    std::uint64_t not_connected{};
    std::uint64_t unavailable{};
    std::uint64_t timeout{};
    std::uint64_t rejected{};
    std::uint64_t invalid_point{};
    std::uint64_t publish_errors{};    // publish() threw (unexpected)
    std::uint64_t stopped_in_flight{}; // the point whose publish() returned Stopped at shutdown
    std::uint64_t shutdown_dropped{};  // points still queued, never attempted, at shutdown
    core::QueueStats queue{};
};

// Per configured node: DataChanges without a value and the most recent raw status code.
struct ItemDiagnostics
{
    std::string asset;
    std::string tag;
    std::string node_id;
    std::uint64_t no_value{};
    std::optional<opcua::StatusCode> last_no_value_status;
};

// Startup validation failure (configuration not usable by the MVP 1 runtime).
class StartupError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class GatewayRuntime
{
public:
    // Builds the item table and the queue; no threads and no network activity.
    // Throws std::invalid_argument for unusable input (no nodes, empty adapters, zero capacity).
    GatewayRuntime(const config::GatewayConfig& config, std::unique_ptr<opcua::IOpcUaClient> opcua,
                   std::unique_ptr<output::ITelemetryPublisher> publisher, RuntimeOptions options);
    // Calls stop().
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;
    GatewayRuntime(GatewayRuntime&&) = delete;
    GatewayRuntime& operator=(GatewayRuntime&&) = delete;

    // Starts the monitor thread, the publisher, the consumer thread and finally the OPC UA
    // client (producers last). On failure everything already started is stopped in reverse
    // order and the exception is rethrown. Call at most once.
    void start();

    // Bounded shutdown: stop producers -> close the queue -> drain the consumer until it is done
    // or drain_timeout passes (then the publisher is stopped to interrupt a blocked publish) ->
    // stop the publisher -> join. Idempotent; must not be called from an adapter callback.
    void stop() noexcept;

    [[nodiscard]] RuntimeStats stats() const;
    [[nodiscard]] std::vector<ItemDiagnostics> item_diagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Rejects configurations the MVP 1 runtime cannot honour (e.g. MQTT QoS other than 1).
// Throws StartupError listing every problem.
void validate_for_mvp1(const config::GatewayConfig& config);

[[nodiscard]] RuntimeOptions runtime_options_from(const config::GatewayConfig& config);

// Production wiring: validate_for_mvp1(), then an Open62541Client and an MqttPublisher built from
// the configuration. Throws StartupError for invalid configuration or adapter options.
[[nodiscard]] std::unique_ptr<GatewayRuntime>
make_gateway_runtime(const config::GatewayConfig& config);

} // namespace ieg::runtime
