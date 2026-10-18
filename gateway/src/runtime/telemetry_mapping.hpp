#pragma once

#include "ieg/config/gateway_config.hpp"
#include "ieg/domain/telemetry.hpp"
#include "ieg/opcua/opcua_client.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ieg::runtime::detail {

inline constexpr std::string_view kOpcUaProtocol = "opcua";

struct ItemBinding
{
    std::string asset;
    std::string tag;
    std::string unit;
    std::string node_id;
};

// Immutable after construction, so lookups need no locking. ItemId == index in configuration
// order (assets in order, then their nodes in order).
class ItemTable
{
public:
    // Throws std::invalid_argument if the configuration has no nodes.
    explicit ItemTable(const config::GatewayConfig& config);

    [[nodiscard]] const std::vector<opcua::MonitoredItem>& monitored_items() const noexcept
    {
        return items_;
    }
    [[nodiscard]] const ItemBinding* find(opcua::ItemId id) const noexcept
    {
        return id < bindings_.size() ? &bindings_[id] : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }

private:
    std::vector<ItemBinding> bindings_;
    std::vector<opcua::MonitoredItem> items_;
};

// Gateway receipt time with the domain Timestamp resolution.
[[nodiscard]] domain::Timestamp receipt_time() noexcept;

// nullopt if the DataChange carries no value (it must not become an invalid TelemetryPoint).
// Otherwise value, quality and source timestamp are taken as delivered (quality is never
// normalized) and edge_timestamp is the given receipt time.
[[nodiscard]] std::optional<domain::TelemetryPoint> make_point(const ItemBinding& binding,
                                                               const opcua::DataChange& change,
                                                               domain::Timestamp edge_timestamp);

} // namespace ieg::runtime::detail
