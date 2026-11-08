#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/config/gateway_config.hpp"
#include "ieg/core/bounded_queue.hpp"
#include "ieg/output/telemetry_publisher.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Gateway runtime: wires an acquisition source (producer), the bounded telemetry queue and a
// telemetry publisher (single consumer thread) together, with bounded shutdown. The runtime
// depends only on the SDK-neutral IAcquisitionSource and ITelemetryPublisher interfaces.
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
    std::uint64_t received{};      // acquisition updates
    std::uint64_t enqueued{};      // points accepted by the queue
    std::uint64_t queue_dropped{}; // oldest points evicted because the queue was full
    std::uint64_t no_value{};      // updates without a value (no TelemetryPoint created)
    std::uint64_t unknown_item{};  // updates for a (source_id, item_id) never requested
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

// Per configured item: updates without a value and the most recent raw protocol status of such
// an update (for OPC UA the StatusCode, e.g. 0x80340000 BadNodeIdUnknown).
struct ItemDiagnostics
{
    std::string asset;
    std::string tag;
    std::string source_id;
    // Adapter-native locator (the OPC UA NodeId).
    std::string locator;
    std::uint64_t no_value{};
    std::optional<acquisition::NativeStatusCode> last_no_value_status;
};

// Startup validation failure (configuration not usable by this runtime).
class StartupError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class GatewayRuntime
{
public:
    // Builds the item table and the queue; no threads and no network activity.
    // Every configured node is requested from `source`; updates are accepted only for that
    // source's id. Throws std::invalid_argument for unusable input (no OPC UA nodes, configured
    // Modbus devices, empty adapters, zero capacity).
    GatewayRuntime(const config::GatewayConfig& config,
                   std::unique_ptr<acquisition::IAcquisitionSource> source,
                   std::unique_ptr<output::ITelemetryPublisher> publisher, RuntimeOptions options);
    // Calls stop().
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;
    GatewayRuntime(GatewayRuntime&&) = delete;
    GatewayRuntime& operator=(GatewayRuntime&&) = delete;

    // Starts the monitor thread, the publisher, the consumer thread and finally the acquisition
    // source (producers last). On failure everything already started (and the source, whose
    // stop() is safe after a failed start()) is stopped in reverse order and the exception is
    // rethrown. Call at most once.
    void start();

    // Bounded shutdown: stop the source (producers) -> close the queue -> drain the consumer
    // until it is done or drain_timeout passes (then the publisher is stopped to interrupt a
    // blocked publish) -> stop the publisher -> join. Idempotent; must not be called from an
    // adapter callback.
    void stop() noexcept;

    [[nodiscard]] RuntimeStats stats() const;
    [[nodiscard]] std::vector<ItemDiagnostics> item_diagnostics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Rejects configurations this runtime cannot honour: MQTT QoS other than 1, any Modbus TCP device
// (parsed and validated by the loader, but acquisition is not implemented yet) and configurations
// without the OPC UA source. Throws StartupError listing every problem.
void validate_for_mvp1(const config::GatewayConfig& config);

[[nodiscard]] RuntimeOptions runtime_options_from(const config::GatewayConfig& config);

// Production wiring: validate_for_mvp1(), then an OpcUaSource wrapping an Open62541Client and an
// MqttPublisher, all built from the configuration. Throws StartupError for invalid configuration or
// adapter options.
[[nodiscard]] std::unique_ptr<GatewayRuntime>
make_gateway_runtime(const config::GatewayConfig& config);

} // namespace ieg::runtime
