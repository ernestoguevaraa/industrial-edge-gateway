#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/opcua/opcua_client.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ieg::acquisition {

// Bridges an IOpcUaClient to the SDK-neutral acquisition ingress. Ownership of the client is
// exclusive; no open62541 type escapes this boundary. Reconnection and callback quiescence on
// stop() are provided by the wrapped client. The raw OPC UA StatusCode is forwarded unchanged as
// Update::native_status.
class OpcUaSource final : public IAcquisitionSource
{
public:
    // Throws std::invalid_argument for an empty source ID or a null client.
    OpcUaSource(std::string source_id, std::unique_ptr<opcua::IOpcUaClient> client);
    ~OpcUaSource() override;

    OpcUaSource(const OpcUaSource&) = delete;
    OpcUaSource& operator=(const OpcUaSource&) = delete;
    OpcUaSource(OpcUaSource&&) = delete;
    OpcUaSource& operator=(OpcUaSource&&) = delete;

    [[nodiscard]] const std::string& id() const noexcept override { return source_id_; }
    void start(std::vector<Item> items, Callbacks callbacks) override;
    void stop() noexcept override;

private:
    enum class Phase : std::uint8_t
    {
        Idle,
        Started,
        Stopped
    };

    const std::string source_id_;
    const std::unique_ptr<opcua::IOpcUaClient> client_;
    std::mutex lifecycle_mutex_;
    Phase phase_{Phase::Idle};
};

} // namespace ieg::acquisition
