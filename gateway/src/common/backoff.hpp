#pragma once

#include <chrono>

namespace ieg::detail {

// Reconnect delay after `failures` consecutive failures: min(initial * 2^failures, max).
// Saturates instead of overflowing for any number of failures.
[[nodiscard]] constexpr std::chrono::milliseconds
backoff_delay(unsigned failures, std::chrono::milliseconds initial,
              std::chrono::milliseconds max) noexcept
{
    if (initial >= max) {
        return max;
    }
    auto delay = initial;
    for (unsigned i = 0; i < failures; ++i) {
        if (delay >= max / 2) {
            return max;
        }
        delay *= 2;
    }
    return delay;
}

} // namespace ieg::detail
