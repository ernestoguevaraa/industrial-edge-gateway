#include "runtime/telemetry_mapping.hpp"

#include "ieg/modbus/modbus_tcp_source.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ieg::runtime::detail {
namespace {

acquisition::ItemId next_id(const std::vector<acquisition::Item>& items)
{
    if (items.size() >= std::numeric_limits<acquisition::ItemId>::max()) {
        throw std::invalid_argument("too many configured points for one source");
    }
    return static_cast<acquisition::ItemId>(items.size());
}

} // namespace

ItemTable::ItemTable(const config::GatewayConfig& config)
{
    if (config.opcua) {
        SourceSpec spec{.source_id = std::string{kOpcUaSourceId},
                        .protocol = std::string{kOpcUaProtocol},
                        .target = config.opcua->endpoint,
                        .items = {}};
        std::vector<ItemBinding> bindings;
        for (const auto& asset : config.assets) {
            for (const auto& node : asset.nodes) {
                spec.items.push_back(
                    acquisition::Item{.id = next_id(spec.items), .locator = node.node_id});
                bindings.push_back(ItemBinding{.asset = asset.asset,
                                               .tag = node.tag,
                                               .unit = node.unit,
                                               .locator = node.node_id,
                                               .protocol = spec.protocol,
                                               .source = 0});
            }
        }
        add_source(std::move(spec), std::move(bindings));
    }
    for (const auto& device : config.modbus.devices) {
        SourceSpec spec{.source_id = device.source_id,
                        .protocol = std::string{kModbusTcpProtocol},
                        .target = device.host + ":" + std::to_string(device.port),
                        .items = {}};
        std::vector<ItemBinding> bindings;
        for (const auto& point : device.points) {
            const auto locator = modbus::modbus_point_locator(point);
            spec.items.push_back(acquisition::Item{.id = next_id(spec.items), .locator = locator});
            bindings.push_back(ItemBinding{.asset = point.asset,
                                           .tag = point.tag,
                                           .unit = point.unit,
                                           .locator = locator,
                                           .protocol = spec.protocol,
                                           .source = 0});
        }
        add_source(std::move(spec), std::move(bindings));
    }
    if (bindings_.empty()) {
        throw std::invalid_argument("configuration contains no acquisition points");
    }
}

void ItemTable::add_source(SourceSpec spec, std::vector<ItemBinding> bindings)
{
    if (spec.items.empty()) {
        return; // a source without points is never started
    }
    const std::size_t index = sources_.size();
    if (!by_source_id_.emplace(spec.source_id, index).second) {
        throw std::invalid_argument("duplicate acquisition source ID '" + spec.source_id + "'");
    }
    first_.push_back(bindings_.size());
    for (auto& binding : bindings) {
        binding.source = index;
        bindings_.push_back(std::move(binding));
    }
    sources_.push_back(std::move(spec));
}

const ItemBinding* ItemTable::find(const std::string& source_id, acquisition::ItemId item) const
{
    const auto found = by_source_id_.find(source_id);
    if (found == by_source_id_.end() || item >= sources_[found->second].items.size()) {
        return nullptr;
    }
    return &bindings_[first_[found->second] + item];
}

std::optional<std::size_t> ItemTable::source_index(const std::string& source_id) const
{
    const auto found = by_source_id_.find(source_id);
    return found == by_source_id_.end() ? std::nullopt : std::optional{found->second};
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
