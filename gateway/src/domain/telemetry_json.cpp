#include "ieg/domain/telemetry_json.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace ieg::domain {

namespace {

nlohmann::ordered_json value_to_json(const TelemetryPoint& point)
{
    return std::visit(
        [&point](const auto& value) -> nlohmann::ordered_json {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, double>) {
                if (!std::isfinite(value)) {
                    throw std::domain_error(std::format(
                        "cannot serialize non-finite value {} for asset '{}' tag '{}' to JSON",
                        value, point.asset, point.tag));
                }
            }
            return value;
        },
        point.value);
}

} // namespace

std::string format_timestamp(Timestamp timestamp)
{
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(timestamp));
}

nlohmann::ordered_json to_json(const TelemetryPoint& point)
{
    nlohmann::ordered_json json;
    json["asset"] = point.asset;
    json["tag"] = point.tag;
    json["value"] = value_to_json(point);
    json["unit"] = point.unit;
    json["quality"] = to_string(point.quality);
    json["source_timestamp"] =
        point.source_timestamp ? nlohmann::ordered_json(format_timestamp(*point.source_timestamp))
                               : nlohmann::ordered_json(nullptr);
    json["edge_timestamp"] = format_timestamp(point.edge_timestamp);
    json["protocol"] = point.protocol;
    return json;
}

} // namespace ieg::domain
