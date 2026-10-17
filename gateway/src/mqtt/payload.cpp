#include "mqtt/payload.hpp"

#include "ieg/domain/telemetry_json.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace ieg::mqtt::detail {

Payload encode_payload(const domain::TelemetryPoint& point)
{
    try {
        return {.json = domain::to_json(point).dump(), .error = {}};
    } catch (const std::domain_error& error) {
        return {.json = {}, .error = std::string{"cannot serialize point: "} + error.what()};
    } catch (const nlohmann::json::exception& error) {
        return {.json = {}, .error = std::string{"cannot serialize point: "} + error.what()};
    }
}

} // namespace ieg::mqtt::detail
