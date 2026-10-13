# Industrial Edge Gateway

A production-oriented **C++20 Industrial IoT edge gateway** that connects OPC UA devices to MQTT using a normalized telemetry model.

The MVP demonstrates a realistic OT-to-IT data path:

```text
Industrial Plant Simulator
        |
      OPC UA
        |
        v
C++20 Industrial Edge Gateway
  - open62541 client
  - telemetry normalization
  - configuration
  - structured logging
        |
      MQTT 5
        |
        v
Eclipse Mosquitto
```

## Why this project exists

Industrial integration software must do more than move bytes between protocols. It must preserve timestamps, data quality, asset context, configuration, observability, and predictable behavior during failures.

This repository is intended as a portfolio-grade implementation of those concerns.

## MVP 1

MVP 1 intentionally focuses on one complete vertical slice:

- Simulated industrial OPC UA server
- C++20 gateway
- OPC UA subscriptions using `open62541`
- Protocol-independent telemetry model
- MQTT 5 publication
- YAML configuration
- Structured logging
- Docker Compose demo
- Unit and integration tests
- GitHub Actions build/test workflow

Not included in MVP 1:

- Modbus TCP
- Store-and-forward
- SQLite persistence
- Sparkplug B
- ISA-95 hierarchy beyond basic asset identifiers
- TLS/mTLS
- Grafana / InfluxDB / Prometheus

These are planned for later milestones.

## Example telemetry

```json
{
  "asset": "site01.line01.oven01",
  "tag": "temperature",
  "value": 842.4,
  "unit": "degC",
  "quality": "GOOD",
  "source_timestamp": "2026-10-13T16:10:27.242Z",
  "edge_timestamp": "2026-10-13T16:10:27.249Z",
  "protocol": "opcua"
}
```

## Planned demo

```bash
docker compose up --build
```

Then:

```bash
mosquitto_sub -h localhost -t 'industrial/#' -v
```

Expected output:

```text
industrial/site01/line01/press01/pressure {...}
industrial/site01/line01/oven01/temperature {...}
```

## Engineering goals

- Modern C++20
- Clear ownership and lifecycle
- Protocol adapters isolated from the domain model
- Reproducible local environment
- Testable behavior
- Industrially credible telemetry semantics
- Documentation of architecture decisions

See:

- `docs/ARCHITECTURE.md`
- `docs/RUNBOOK.md`
- `docs/ISSUES.md`
- `docs/adr/`
