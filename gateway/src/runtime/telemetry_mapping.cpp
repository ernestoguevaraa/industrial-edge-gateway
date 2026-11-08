#include "runtime/telemetry_mapping.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace ieg::runtime::detail {

ItemTable::ItemTable(const config::GatewayConfig& config)
{
    for (const auto& asset : config.assets) {
        for (const auto& node : asset.nodes) {
            if (bindings_.size() >= std::numeric_limits<acquisition::ItemId>::max()) {
                throw std::invalid_argument("too many configured nodes");
            }
            const auto id = static_cast<acquisition::ItemId>(bindings_.size());
            bindings_.push_back(ItemBinding{.asset = asset.asset,
                                            .tag = node.tag,
                                            .unit = node.unit,
                                            .locator = node.node_id,
                                            .protocol = std::string{kOpcUaProtocol}});
            items_.push_back(acquisition::Item{.id = id, .locator = node.node_id});
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
                                                 const acquisition::Update& update,
                                                 domain::Timestamp edge_timestamp)
{
    if (!update.value) {
        return std::nullopt;
    }
    return domain::TelemetryPoint{.asset = binding.asset,
                                  .tag = binding.tag,
                                  .value = *update.value,
                                  .unit = binding.unit,
                                  .quality = update.quality,
                                  .source_timestamp = update.source_timestamp,
                                  .edge_timestamp = edge_timestamp,
                                  .protocol = binding.protocol};
}

} // namespace ieg::runtime::detail
