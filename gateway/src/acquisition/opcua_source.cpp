#include "ieg/acquisition/opcua_source.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ieg::acquisition {
namespace {

ConnectionState map_state(opcua::ConnectionState state) noexcept
{
    switch (state) {
    case opcua::ConnectionState::Connecting:
        return ConnectionState::Connecting;
    case opcua::ConnectionState::Connected:
        return ConnectionState::Connected;
    case opcua::ConnectionState::Disconnected:
        return ConnectionState::Disconnected;
    }
    return ConnectionState::Disconnected;
}

} // namespace

OpcUaSource::OpcUaSource(std::string source_id, std::unique_ptr<opcua::IOpcUaClient> client)
    : source_id_(std::move(source_id)), client_(std::move(client))
{
    if (source_id_.empty() || !client_) {
        throw std::invalid_argument("OpcUaSource requires a source ID and OPC UA client");
    }
}

OpcUaSource::~OpcUaSource() { stop(); }

void OpcUaSource::start(std::vector<Item> items, Callbacks callbacks)
{
    if (started_) {
        throw std::logic_error("OpcUaSource already started");
    }
    if (items.empty() || !callbacks.on_update || !callbacks.on_status) {
        throw std::invalid_argument("OpcUaSource requires items and both callbacks");
    }

    std::unordered_set<ItemId> ids;
    std::vector<opcua::MonitoredItem> monitored;
    monitored.reserve(items.size());
    for (const auto& item : items) {
        if (item.locator.empty() || !ids.insert(item.id).second) {
            throw std::invalid_argument("OpcUaSource received empty locator or duplicate item ID");
        }
        monitored.push_back(opcua::MonitoredItem{.id = item.id, .node_id = item.locator});
    }

    // Capture by value: the underlying client retains callbacks beyond this stack frame.
    opcua::ClientCallbacks forwarded{
        .on_data_change =
            [source_id = source_id_, cb = std::move(callbacks.on_update)](
                const opcua::DataChange& data) {
                cb(Update{.source_id = source_id,
                          .item_id = data.id,
                          .value = data.value,
                          .quality = data.quality,
                          .source_timestamp = data.source_timestamp,
                          .diagnostic = std::to_string(data.status_code)});
            },
        .on_status_change =
            [source_id = source_id_, cb = std::move(callbacks.on_status)](
                const opcua::ConnectionStatus& status) {
                cb(Status{.source_id = source_id,
                          .state = map_state(status.state),
                          .detail = status.detail});
            },
    };
    // The existing client owns reconnection and guarantees stop() callback quiescence.
    client_->start(std::move(monitored), std::move(forwarded));
    started_ = true;
}

void OpcUaSource::stop() noexcept
{
    client_->stop();
}

} // namespace ieg::acquisition
