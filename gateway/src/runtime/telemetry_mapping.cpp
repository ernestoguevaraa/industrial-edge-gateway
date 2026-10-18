#include "runtime/telemetry_mapping.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace ieg::runtime::detail {

ItemTable::ItemTable(const config::GatewayConfig& config)
{
    for (const auto& asset : config.assets) {
        for (const auto& node : asset.nodes) {
            if (bindings_.size() >= std::numeric_limits<opcua::ItemId>::max()) {
                throw std::invalid_argument("too many configured nodes");
            }
            const auto id = static_cast<opcua::ItemId>(bindings_.size());
            bindings_.push_back(ItemBinding{
                .asset = asset.asset, .tag = node.tag, .unit = node.unit, .node_id = node.node_id});
            items_.push_back(opcua::MonitoredItem{.id = id, .node_id = node.node_id});
        }
    }
    if (bindings_.empty()) {
        throw std::invalid_argument("configuration contains no OPC UA nodes");
    }
}

domain::Timestamp receipt_time() noexcept
{
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

std::optional<domain::TelemetryPoint> make_point(const ItemBinding& binding,
                                                 const opcua::DataChange& change,
                                                 domain::Timestamp edge_timestamp)
{
    if (!change.value) {
        return std::nullopt;
    }
    return domain::TelemetryPoint{.asset = binding.asset,
                                  .tag = binding.tag,
                                  .value = *change.value,
                                  .unit = binding.unit,
                                  .quality = change.quality,
                                  .source_timestamp = change.source_timestamp,
                                  .edge_timestamp = edge_timestamp,
                                  .protocol = std::string{kOpcUaProtocol}};
}

} // namespace ieg::runtime::detail
