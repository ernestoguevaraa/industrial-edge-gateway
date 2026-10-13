# ADR-002: Decouple protocol adapters from the telemetry domain model

## Status
Accepted

## Context

MVP 1 receives data through OPC UA and publishes through MQTT.

Future milestones will introduce additional protocols such as Modbus and potentially additional outputs.

Allowing OPC UA or MQTT-specific types to spread through the gateway would make the core difficult to test and extend.

## Decision

Introduce a protocol-neutral `TelemetryPoint` domain model.

OPC UA adapters convert protocol values into the domain model.

MQTT publishers serialize the domain model.

## Consequences

### Positive
- protocol independence
- simpler unit testing
- easier addition of Modbus
- clearer architectural boundaries
- no direct open62541-to-MQTT coupling

### Negative
- explicit conversion layer required
- some protocol-specific metadata may need extension mechanisms later
