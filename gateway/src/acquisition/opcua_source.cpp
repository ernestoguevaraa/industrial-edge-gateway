#include "ieg/acquisition/opcua_source.hpp"

#include <exception>
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

std::vector<opcua::MonitoredItem> to_monitored_items(const std::vector<Item>& items)
{
    if (items.empty()) {
        throw std::invalid_argument("OpcUaSource requires at least one item");
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
    return monitored;
}

} // namespace

OpcUaSource::OpcUaSource(std::string source_id, std::unique_ptr<opcua::IOpcUaClient> client)
    : source_id_(std::move(source_id)), client_(std::move(client))
{
    if (source_id_.empty() || !client_) {
        throw std::invalid_argument("OpcUaSource requires a source ID and OPC UA client");
    }
}

OpcUaSource::~OpcUaSource()
{
    stop();
}

void OpcUaSource::start(std::vector<Item> items, Callbacks callbacks)
{
    const std::lock_guard lock(lifecycle_mutex_);
    if (phase_ != Phase::Idle) {
        throw std::logic_error("OpcUaSource::start() called after start() or stop()");
    }
    if (!callbacks.on_update || !callbacks.on_status) {
        throw std::invalid_argument("OpcUaSource requires both callbacks");
    }
    auto monitored = to_monitored_items(items);

    // Capture by value: the client retains the callbacks beyond this stack frame. The status code
    // is forwarded as an integer, so no conversion (or allocation) happens in the data path.
    opcua::ClientCallbacks forwarded{
        .on_data_change =
            [source_id = source_id_,
             cb = std::move(callbacks.on_update)](const opcua::DataChange& data) {
                cb(Update{.source_id = source_id,
                          .item_id = data.id,
                          .value = data.value,
                          .quality = data.quality,
                          .source_timestamp = data.source_timestamp,
                          .native_status = data.status_code});
            },
        .on_status_change =
            [source_id = source_id_,
             cb = std::move(callbacks.on_status)](const opcua::ConnectionStatus& status) {
                cb(Status{.source_id = source_id,
                          .state = map_state(status.state),
                          .detail = status.detail});
            },
    };
    try {
        client_->start(std::move(monitored), std::move(forwarded));
    } catch (const std::invalid_argument&) {
        throw; // rejected request (e.g. unparsable NodeId): the client did not start
    } catch (...) {
        // Unexpected failure: the client may be partially started. Release it now so no
        // callback can run after start() has thrown; the single start is consumed.
        phase_ = Phase::Stopped;
        client_->stop();
        throw;
    }
    phase_ = Phase::Started;
}

void OpcUaSource::stop() noexcept
{
    try {
        const std::lock_guard lock(lifecycle_mutex_);
        phase_ = Phase::Stopped;
        // Idempotent and safe before start(); returns only once no callback can run.
        client_->stop();
    } catch (...) {
        // Only reachable if locking itself fails; nothing safe is left to do.
        std::terminate();
    }
}

} // namespace ieg::acquisition
