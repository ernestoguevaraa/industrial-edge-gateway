#include "mqtt/topic.hpp"

#include <cstdint>

namespace ieg::mqtt::detail {
namespace {

constexpr std::string_view kTopicSeparators = "/+#";

bool has_forbidden_level_char(std::string_view level) noexcept
{
    return level.find_first_of("+#") != std::string_view::npos ||
           level.find('\0') != std::string_view::npos;
}

bool has_forbidden_segment_char(std::string_view segment) noexcept
{
    return segment.find_first_of(kTopicSeparators) != std::string_view::npos ||
           segment.find('\0') != std::string_view::npos;
}

std::string quoted(std::string_view text)
{
    return "\"" + std::string{text} + "\"";
}

} // namespace

bool is_valid_mqtt_utf8(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (lead == 0x00U) {
            return false;
        }
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<std::uint8_t>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        const bool overlong = (length == 2 && code_point < 0x80U) ||
                              (length == 3 && code_point < 0x800U) ||
                              (length == 4 && code_point < 0x10000U);
        const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;
        if (overlong || surrogate || code_point > 0x10FFFFU) {
            return false;
        }
        i += length;
    }
    return true;
}

std::string topic_prefix_error(std::string_view prefix)
{
    if (prefix.empty()) {
        return "topic prefix must not be empty";
    }
    if (prefix.size() > kMaxTopicBytes) {
        return "topic prefix is longer than 65535 bytes";
    }
    if (!is_valid_mqtt_utf8(prefix)) {
        return "topic prefix is not valid UTF-8 (or contains NUL)";
    }
    std::size_t start = 0;
    while (true) {
        const auto end = prefix.find('/', start);
        const auto level = prefix.substr(start, end == std::string_view::npos ? end : end - start);
        if (level.empty()) {
            return "topic prefix " + quoted(prefix) +
                   " has an empty level (leading, trailing or double '/')";
        }
        if (has_forbidden_level_char(level)) {
            return "topic prefix " + quoted(prefix) + " contains a wildcard ('+' or '#') or NUL";
        }
        if (end == std::string_view::npos) {
            return {};
        }
        start = end + 1;
    }
}

Topic make_topic(std::string_view prefix, std::string_view asset, std::string_view tag)
{
    std::string name{prefix};
    std::size_t start = 0;
    while (true) {
        const auto end = asset.find('.', start);
        const auto segment = asset.substr(start, end == std::string_view::npos ? end : end - start);
        if (segment.empty()) {
            return {.name = {}, .error = "asset " + quoted(asset) + " has an empty level"};
        }
        if (has_forbidden_segment_char(segment)) {
            return {.name = {},
                    .error = "asset " + quoted(asset) + " contains '/', '+', '#' or NUL"};
        }
        name += '/';
        name += segment;
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (tag.empty()) {
        return {.name = {}, .error = "tag must not be empty"};
    }
    if (has_forbidden_segment_char(tag)) {
        return {.name = {}, .error = "tag " + quoted(tag) + " contains '/', '+', '#' or NUL"};
    }
    name += '/';
    name += tag;

    if (name.size() > kMaxTopicBytes) {
        return {.name = {}, .error = "topic is longer than 65535 bytes"};
    }
    if (!is_valid_mqtt_utf8(name)) {
        return {.name = {}, .error = "topic is not valid UTF-8"};
    }
    return {.name = std::move(name), .error = {}};
}

} // namespace ieg::mqtt::detail
