#pragma once

#include <cstdint>
#include <string_view>

namespace ieg::domain {

enum class DataQuality : std::uint8_t
{
    Good,
    Uncertain,
    Bad
};

[[nodiscard]] constexpr std::string_view to_string(DataQuality quality) noexcept
{
    switch (quality) {
    case DataQuality::Good:
        return "GOOD";
    case DataQuality::Uncertain:
        return "UNCERTAIN";
    case DataQuality::Bad:
        return "BAD";
    }
    // Only reachable through an invalid enum cast; never report such a value as usable.
    return "BAD";
}

} // namespace ieg::domain
