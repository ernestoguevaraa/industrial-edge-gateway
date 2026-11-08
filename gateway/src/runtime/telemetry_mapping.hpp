#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/config/gateway_config.hpp"
#include "ieg/domain/telemetry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ieg::runtime::detail {

// Normalized protocol identifier published in TelemetryPoint::protocol for OPC UA items.
inline constexpr std::string_view kOpcUaProtocol = "opcua";
// Source ID of the single OPC UA source configured by the MVP 1 YAML schema.
inline constexpr std::string_view kOpcUaSourceId = "opcua-main";

struct ItemBinding
{
    std::string asset;
    std::string tag;
    std::string unit;
    // Adapter-native locator (the OPC UA NodeId for configured OPC UA nodes).
    std::string locator;
    std::string protocol;
};

// Immutable after construction, so lookups need no locking. ItemId == index in configuration
// order (assets in order, then their nodes in order).
class ItemTable
{
public:
    // Throws std::invalid_argument if the configuration has no nodes.
    explicit ItemTable(const config::GatewayConfig& config);

    [[nodiscard]] const std::vector<acquisition::Item>& items() const noexcept { return items_; }
    [[nodiscard]] const ItemBinding* find(acquisition::ItemId id) const noexcept
    {
        return id < bindings_.size() ? &bindings_[id] : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept { return bindings_.size(); }

private:
    std::vector<ItemBinding> bindings_;
    std::vector<acquisition::Item> items_;
};

// Gateway receipt time with the domain Timestamp resolution.
[[nodiscard]] domain::Timestamp receipt_time() noexcept;

// nullopt if the update carries no value (it must not become an invalid TelemetryPoint).
// Otherwise value, quality and source timestamp are taken as delivered (quality is never
// normalized), protocol comes from the binding and edge_timestamp is the given receipt time.
[[nodiscard]] std::optional<domain::TelemetryPoint> make_point(const ItemBinding& binding,
                                                               const acquisition::Update& update,
                                                               domain::Timestamp edge_timestamp);

} // namespace ieg::runtime::detail
