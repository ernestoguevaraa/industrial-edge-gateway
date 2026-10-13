# ADR-003: Use MQTT 5 as the initial northbound protocol

## Status
Accepted

## Context

The gateway requires a lightweight northbound protocol suitable for edge-to-IT/cloud integration.

MQTT is widely used in Industrial IoT and aligns with future support for Sparkplug B.

## Decision

Use MQTT 5 for MVP 1.

Initial settings:

- QoS 1
- retain false
- JSON payload
- deterministic topic hierarchy

## Alternatives considered

- MQTT 3.1.1
- AMQP
- HTTP/REST
- gRPC streaming
- Kafka producer

## Consequences

### Positive
- lightweight
- broadly supported
- appropriate for intermittent edge connectivity
- natural evolution path toward Sparkplug B

### Negative
- JSON payload is not schema-enforced
- delivery semantics require explicit design
- persistent store-and-forward is not provided by MQTT alone
