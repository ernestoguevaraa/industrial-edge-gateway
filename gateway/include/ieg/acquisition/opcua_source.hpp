#pragma once

#include "ieg/acquisition/acquisition_source.hpp"
#include "ieg/opcua/opcua_client.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ieg::acquisition {

// Bridges the existing OPC UA client to the SDK-neutral acquisition ingress.
// Ownership is exclusive; no open62541 types escape this boundary.
class OpcUaSource final : public IAcquisitionSource
{
public:
    OpcUaSource(std::string source_id, std::unique_ptr<opcua::IOpcUaClient> client);
    ~OpcUaSource() override;

    void start(std::vector<Item> items, Callbacks callbacks) override;
    void stop() noexcept override;

private:
    std::string source_id_;
    std::unique_ptr<opcua::IOpcUaClient> client_;
    bool started_{false};
};

} // namespace ieg::acquisition
