#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ieg::mqtt::detail {

// MQTT UTF-8 encoded strings are length-prefixed with two bytes.
inline constexpr std::size_t kMaxTopicBytes = 65535;

// Well-formed UTF-8 (no overlong forms, surrogates or code points above U+10FFFF) and no U+0000,
// as required for MQTT UTF-8 encoded strings.
[[nodiscard]] bool is_valid_mqtt_utf8(std::string_view text) noexcept;

// Empty if `prefix` is a valid topic prefix: one or more levels separated by '/', no
// leading/trailing slash, no empty level, no '+', '#' or NUL, valid UTF-8. Otherwise the reason.
[[nodiscard]] std::string topic_prefix_error(std::string_view prefix);

struct Topic
{
    std::string name;  // valid only if error is empty
    std::string error; // reason the point cannot be mapped to a topic

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// <prefix>/<asset split on '.'>/<tag>, e.g. "industrial/site01/line01/press01/pressure".
// Asset levels and the tag must be non-empty and must not contain '/', '+', '#' or NUL. The
// final topic must be valid UTF-8 and at most kMaxTopicBytes long. `prefix` must already be
// valid (see topic_prefix_error).
[[nodiscard]] Topic make_topic(std::string_view prefix, std::string_view asset,
                               std::string_view tag);

} // namespace ieg::mqtt::detail
