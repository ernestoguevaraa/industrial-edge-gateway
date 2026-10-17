#pragma once

#include "ieg/domain/telemetry.hpp"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// Protocol-neutral output abstraction used by the gateway core. Concrete publishers (MQTT in
// Issue #9) live behind it; no MQTT or other protocol library type may appear in this header.
// Connection management, including automatic reconnect, is an implementation responsibility.
namespace ieg::output {

enum class PublishStatus : std::uint8_t
{
    // Accepted per the configured delivery guarantee (for MQTT QoS 1: PUBACK received).
    Delivered,
    // There is no usable connection; the point was definitely not submitted. Returned
    // immediately without waiting for a reconnect. Retryable. In MVP 1 the caller has no outage
    // buffering, so this point may be lost.
    NotConnected,
    // The publisher is running and may still be connected, but a transient local/runtime
    // condition (e.g. too many messages in flight) prevented submission; the point was
    // definitely not submitted. Retryable.
    Unavailable,
    // Submitted, but the delivery acknowledgement is unknown (none within the publish timeout,
    // or the connection was lost first). Retryable; retrying gives at-least-once delivery
    // (possible duplicates).
    Timeout,
    // The receiving side refused this publication. Whether a retry can help depends on the
    // concrete reason, so the publisher decides the RetryDisposition.
    Rejected,
    // The point can never be published as-is (e.g. non-finite double, no valid destination).
    InvalidPoint,
    // The publisher is stopped or stopping; not delivered.
    Stopped
};

enum class RetryDisposition : std::uint8_t
{
    // Nothing to retry (the point was delivered).
    NotApplicable,
    // A later attempt with the same point may succeed.
    Retryable,
    // Retrying the same point cannot succeed.
    Permanent
};

[[nodiscard]] constexpr std::string_view to_string(PublishStatus status) noexcept
{
    switch (status) {
    case PublishStatus::Delivered:
        return "Delivered";
    case PublishStatus::NotConnected:
        return "NotConnected";
    case PublishStatus::Unavailable:
        return "Unavailable";
    case PublishStatus::Timeout:
        return "Timeout";
    case PublishStatus::Rejected:
        return "Rejected";
    case PublishStatus::InvalidPoint:
        return "InvalidPoint";
    case PublishStatus::Stopped:
        return "Stopped";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view to_string(RetryDisposition retry) noexcept
{
    switch (retry) {
    case RetryDisposition::NotApplicable:
        return "NotApplicable";
    case RetryDisposition::Retryable:
        return "Retryable";
    case RetryDisposition::Permanent:
        return "Permanent";
    }
    return "Unknown";
}

// Outcome of one publish() call. Status and retry disposition are deliberately separate: the
// disposition for Rejected depends on the concrete publisher and the actual rejection reason.
// Use the factories so status and disposition stay consistent:
//   Delivered -> NotApplicable, NotConnected/Unavailable/Timeout/Stopped -> Retryable,
//   InvalidPoint -> Permanent, Rejected -> chosen by the publisher (Retryable or Permanent).
struct PublishResult
{
    PublishStatus status{PublishStatus::NotConnected};
    RetryDisposition retry{RetryDisposition::Retryable};
    // Human-readable context for logs (e.g. a protocol reason code); may be empty.
    std::string detail;

    [[nodiscard]] bool delivered() const noexcept { return status == PublishStatus::Delivered; }
    [[nodiscard]] bool retryable() const noexcept { return retry == RetryDisposition::Retryable; }

    // For every status except Rejected, which needs an explicit disposition (see rejected()).
    // Throws std::invalid_argument for Rejected.
    [[nodiscard]] static PublishResult of(PublishStatus status, std::string detail = {})
    {
        RetryDisposition retry = RetryDisposition::Retryable;
        switch (status) {
        case PublishStatus::Delivered:
            retry = RetryDisposition::NotApplicable;
            break;
        case PublishStatus::NotConnected:
        case PublishStatus::Unavailable:
        case PublishStatus::Timeout:
        case PublishStatus::Stopped:
            retry = RetryDisposition::Retryable;
            break;
        case PublishStatus::InvalidPoint:
            retry = RetryDisposition::Permanent;
            break;
        case PublishStatus::Rejected:
            throw std::invalid_argument(
                "PublishResult::of(Rejected): use PublishResult::rejected(disposition, detail)");
        }
        return PublishResult{.status = status, .retry = retry, .detail = std::move(detail)};
    }

    // A rejection whose disposition the concrete publisher derived from the actual reason.
    // Throws std::invalid_argument if retry is NotApplicable.
    [[nodiscard]] static PublishResult rejected(RetryDisposition retry, std::string detail)
    {
        if (retry == RetryDisposition::NotApplicable) {
            throw std::invalid_argument(
                "PublishResult::rejected(): disposition must be Retryable or Permanent");
        }
        return PublishResult{
            .status = PublishStatus::Rejected, .retry = retry, .detail = std::move(detail)};
    }

    friend bool operator==(const PublishResult&, const PublishResult&) = default;
};

enum class PublisherState : std::uint8_t
{
    // First connection attempt, or a new attempt after Disconnected.
    Connecting,
    // Connected and able to publish.
    Connected,
    // Operational connectivity failure or loss while the publisher is active; it keeps
    // retrying. Never reported for a deliberate stop().
    Disconnected
};

struct PublisherStatus
{
    PublisherState state{PublisherState::Disconnected};
    // Human-readable reason; may be empty.
    std::string detail;

    friend bool operator==(const PublisherStatus&, const PublisherStatus&) = default;
};

// Runs on the publisher's internal thread, serialized. Must be short and non-blocking and must
// not call publish() or stop(). The argument is valid only for the duration of the call.
using PublisherStatusCallback = std::function<void(const PublisherStatus&)>;

class ITelemetryPublisher
{
public:
    ITelemetryPublisher() = default;
    // Defaulted: it does NOT stop the publisher. Concrete implementations must call stop() from
    // their own destructor.
    virtual ~ITelemetryPublisher() = default;

    ITelemetryPublisher(const ITelemetryPublisher&) = delete;
    ITelemetryPublisher& operator=(const ITelemetryPublisher&) = delete;
    ITelemetryPublisher(ITelemetryPublisher&&) = delete;
    ITelemetryPublisher& operator=(ITelemetryPublisher&&) = delete;

    // Starts connection management (connect, then reconnect with bounded backoff after any loss)
    // and returns without waiting for the network. Call at most once per instance.
    // Throws std::invalid_argument if on_status is empty and std::logic_error if already started
    // (or started after stop()). Connectivity problems are reported through on_status, never
    // thrown. on_status must stay valid until stop() returns.
    virtual void start(PublisherStatusCallback on_status) = 0;

    // Publishes one point. Synchronous from the caller's perspective: returns once the outcome
    // is known, bounded by the implementation's publish timeout. Returns NotConnected
    // immediately while disconnected (never waits for a reconnect) and Stopped before start() or
    // after stop(). Never throws for connectivity, broker or data problems. The point is not
    // referenced after return.
    //
    // Single-consumer contract: calls must not overlap (one consumer thread at a time). Never
    // call from a status callback or from a protocol adapter thread (e.g. an OPC UA callback).
    virtual PublishResult publish(const domain::TelemetryPoint& point) = 0;

    // Disconnects gracefully and releases resources. Idempotent and safe if start() was never
    // called. May run concurrently with a blocked publish(), which then returns Stopped
    // promptly. Does not report Disconnected. When it returns, no status callback is running and
    // none will run afterwards. Must not be called from inside a status callback.
    virtual void stop() noexcept = 0;
};

} // namespace ieg::output
