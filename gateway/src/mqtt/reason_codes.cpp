#include "mqtt/reason_codes.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace ieg::mqtt::detail {
namespace {

using output::PublishResult;
using output::PublishStatus;
using output::RetryDisposition;

std::string hex_code(int reason_code)
{
    std::array<char, 8> buffer{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fixed format, bounded buffer.
    static_cast<void>(std::snprintf(buffer.data(), buffer.size(), "0x%02X",
                                    static_cast<unsigned>(reason_code) & 0xFFU));
    return buffer.data();
}

std::string describe(int reason_code, std::string_view name, std::string_view reason_string)
{
    std::string detail = "PUBACK " + hex_code(reason_code) + " " + std::string{name};
    if (!reason_string.empty()) {
        detail += ": ";
        detail += reason_string;
    }
    return detail;
}

} // namespace

PubackMapping map_puback_reason(int reason_code, std::string_view reason_string)
{
    const auto rejected = [&](RetryDisposition retry, std::string_view name) {
        return PubackMapping{
            .result = PublishResult::rejected(retry, describe(reason_code, name, reason_string)),
            .unexpected = false};
    };
    switch (reason_code) {
    case 0x00:
        return {.result = PublishResult::of(PublishStatus::Delivered), .unexpected = false};
    case 0x10:
        return {.result = PublishResult::of(PublishStatus::Delivered,
                                            describe(reason_code, "No matching subscribers", {})),
                .unexpected = false};
    case 0x87:
        return rejected(RetryDisposition::Permanent, "Not authorized");
    case 0x90:
        return rejected(RetryDisposition::Permanent, "Topic Name invalid");
    case 0x99:
        return rejected(RetryDisposition::Permanent, "Payload format invalid");
    case 0x80:
        return rejected(RetryDisposition::Retryable, "Unspecified error");
    case 0x83:
        return rejected(RetryDisposition::Retryable, "Implementation specific error");
    case 0x91:
        return rejected(RetryDisposition::Retryable, "Packet Identifier in use");
    case 0x97:
        return rejected(RetryDisposition::Retryable, "Quota exceeded");
    default:
        return {.result = PublishResult::rejected(
                    RetryDisposition::Retryable,
                    describe(reason_code, "unexpected reason code", reason_string)),
                .unexpected = true};
    }
}

} // namespace ieg::mqtt::detail
