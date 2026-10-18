#pragma once

#include "ieg/core/bounded_queue.hpp"
#include "ieg/domain/telemetry.hpp"

namespace ieg::core {

// Hand-off between protocol callbacks and the publishing worker. The gateway core stamps
// edge_timestamp before try_push(); the capacity is chosen by the runtime/configuration layer.
// The queue itself is only an in-memory decoupling primitive. The MVP 1 runtime's consumer stops
// popping while MQTT is disconnected, so the queue also acts as a bounded, best-effort buffer for
// short outages (drop-oldest beyond capacity; lost on restart). It is not store-and-forward and
// not a delivery guarantee.
using TelemetryQueue = BoundedQueue<domain::TelemetryPoint>;

} // namespace ieg::core
