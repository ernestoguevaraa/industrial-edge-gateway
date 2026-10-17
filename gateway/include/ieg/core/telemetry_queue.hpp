#pragma once

#include "ieg/core/bounded_queue.hpp"
#include "ieg/domain/telemetry.hpp"

namespace ieg::core {

// Hand-off between protocol callbacks and the publishing worker. The gateway core stamps
// edge_timestamp before try_push(); the capacity is chosen by the runtime/configuration layer.
// In-memory only: not store-and-forward and not an outage-delivery guarantee.
using TelemetryQueue = BoundedQueue<domain::TelemetryPoint>;

} // namespace ieg::core
