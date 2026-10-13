#pragma once

#include "ieg/domain/data_quality.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace ieg::domain {

using TelemetryValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

// UTC time point with nanosecond resolution (OPC UA DateTime is 100 ns).
using Timestamp = std::chrono::sys_time<std::chrono::nanoseconds>;

struct TelemetryPoint
{
    std::string asset;
    std::string tag;
    TelemetryValue value;
    std::string unit;
    DataQuality quality{DataQuality::Bad};
    // Absent when the source did not provide one; never substituted with another timestamp.
    std::optional<Timestamp> source_timestamp;
    Timestamp edge_timestamp;
    // Normalized lowercase protocol identifier, e.g. "opcua" or "modbus".
    std::string protocol;

    friend bool operator==(const TelemetryPoint&, const TelemetryPoint&) = default;
};

} // namespace ieg::domain
