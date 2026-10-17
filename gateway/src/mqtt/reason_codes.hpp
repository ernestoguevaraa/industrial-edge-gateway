#pragma once

#include "ieg/output/telemetry_publisher.hpp"

#include <string_view>

namespace ieg::mqtt::detail {

struct PubackMapping
{
    output::PublishResult result;
    // The reason code is not one of the explicitly supported PUBACK codes.
    bool unexpected{false};
};

// Maps an MQTT 5 PUBACK reason code (OASIS MQTT 5.0, 3.4.2.1) to a PublishResult.
// Delivered only for 0x00 Success and 0x10 No matching subscribers. Explicitly supported
// rejections get their retry disposition from the reason; any other code, including undefined
// values below 0x80, becomes Rejected / Retryable with `unexpected` set.
// `reason_string` is the optional MQTT 5 Reason String property, appended to the detail.
[[nodiscard]] PubackMapping map_puback_reason(int reason_code, std::string_view reason_string = {});

} // namespace ieg::mqtt::detail
