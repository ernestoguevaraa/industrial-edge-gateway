#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/config/gateway_config.hpp"
#include "ieg/domain/telemetry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ieg::runtime::detail {

// Normalized protocol identifiers published in TelemetryPoint::protocol.
inline constexpr std::string_view kOpcUaProtocol = "opcua";
inline constexpr std::string_view kModbusTcpProtocol = "modbus_tcp";
// Source ID of the single OPC UA source configured by the "opcua" section.
inline constexpr std::string_view kOpcUaSourceId = config::kOpcUaSourceId;

struct ItemBinding
{
    std::string asset;
    std::string tag;
    std::string unit;
    // Adapter-native locator: the OPC UA NodeId, or "<asset>/<tag>" for a Modbus point.
    std::string locator;
    std::string protocol;
    // Index of the owning source in ItemTable::sources().
    std::size_t source{};
};

// One configured acquisition source and the items the runtime requests from it.
struct SourceSpec
{
    std::string source_id;
    std::string protocol;
    // Endpoint for logs: the OPC UA endpoint URL or "<host>:<port>" of a Modbus device.
    std::string target;
    // ItemId == position in this list (0..n-1), so IDs are unique per source only.
    std::vector<acquisition::Item> items;
};

// Every configured point, keyed by (source_id, item_id). Immutable after construction, so
// lookups from concurrent source callbacks need no locking.
//
// Sources: the OPC UA source (if configured) first, with item IDs in configuration order (assets
// in order, then their nodes) exactly as in MVP 1; then one source per Modbus device in
// configuration order, with item IDs in point order.
class ItemTable
{
public:
    // Throws std::invalid_argument if the configuration has no point at all.
    explicit ItemTable(const config::GatewayConfig& config);

    [[nodiscard]] const std::vector<SourceSpec>& sources() const noexcept { return sources_; }

    // nullptr for an unknown source ID or an item the runtime never requested from that source.
    [[nodiscard]] const ItemBinding* find(const std::string& source_id,
                                          acquisition::ItemId item) const;

    // Global position of a binding (0..size()-1), stable for per-item counters.
    [[nodiscard]] std::size_t index_of(const ItemBinding& binding) const noexcept
    {
        return static_cast<std::size_t>(&binding - bindings_.data());
    }
    [[nodiscard]] const ItemBinding& at(std::size_t index) const { return bindings_.at(index); }
    [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }

    // Index of a source in sources(), or nullopt if no source has this ID.
    [[nodiscard]] std::optional<std::size_t> source_index(const std::string& source_id) const;

private:
    void add_source(SourceSpec spec, std::vector<ItemBinding> bindings);

    std::vector<SourceSpec> sources_;
    std::vector<ItemBinding> bindings_; // source by source, in item ID order
    std::vector<std::size_t> first_;    // global index of each source's item 0
    std::unordered_map<std::string, std::size_t> by_source_id_;
};

// Gateway receipt time with the domain Timestamp resolution.
[[nodiscard]] domain::Timestamp receipt_time() noexcept;

// nullopt if the update carries no value (it must not become an invalid TelemetryPoint).
// Otherwise value, quality and source timestamp are taken as delivered (quality is never
// normalized, an absent source timestamp stays absent), protocol comes from the binding and
// edge_timestamp is the given receipt time.
[[nodiscard]] std::optional<domain::TelemetryPoint> make_point(const ItemBinding& binding,
                                                               const acquisition::Update& update,
                                                               domain::Timestamp edge_timestamp);

} // namespace ieg::runtime::detail
