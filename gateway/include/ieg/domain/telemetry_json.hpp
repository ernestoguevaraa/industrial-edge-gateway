#pragma once

#include "ieg/domain/telemetry.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace ieg::domain {

// ISO 8601 UTC with millisecond precision, truncated towards the past: "2026-10-13T16:10:27.242Z".
[[nodiscard]] std::string format_timestamp(Timestamp timestamp);

// Fields are emitted in declaration order. Throws std::domain_error for non-finite doubles.
[[nodiscard]] nlohmann::ordered_json to_json(const TelemetryPoint& point);

} // namespace ieg::domain
