#pragma once

#include "ieg/domain/telemetry.hpp"

#include <string>

namespace ieg::mqtt::detail {

struct Payload
{
    std::string json;  // valid only if error is empty
    std::string error; // reason the point cannot be serialized

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Compact JSON from domain::to_json (deterministic field order). Non-finite doubles and invalid
// UTF-8 in strings are reported as errors instead of thrown.
[[nodiscard]] Payload encode_payload(const domain::TelemetryPoint& point);

} // namespace ieg::mqtt::detail
