# Industrial Edge Gateway

[![CI](https://github.com/ernestoguevaraa/industrial-edge-gateway/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ernestoguevaraa/industrial-edge-gateway/actions/workflows/ci.yml)

A **C++20 Industrial IoT edge gateway** that connects OPC UA servers to MQTT 5 using a normalized telemetry model.

**Status: v0.1.0 (MVP 1).** A complete OPC UA → MQTT vertical slice and a reference
implementation, not a production-ready product: no authentication or TLS, no persistence.

MVP 1 demonstrates a realistic OT-to-IT data path:

```text
Industrial Plant Simulator
        |
      OPC UA
        |
        v
C++20 Industrial Edge Gateway
  - open62541 OPC UA client
  - telemetry normalization
  - bounded in-memory queue
  - Eclipse Paho MQTT client
  - configuration, structured logging
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

- Simulated industrial OPC UA server (deterministic Python plant simulator)
- C++20 gateway
- OPC UA subscriptions using `open62541`, preserving source timestamps and quality
- Protocol-independent telemetry model
- MQTT 5 publication (QoS 1, retain false) using Eclipse Paho
- Automatic reconnect to the OPC UA server and the MQTT broker
- Bounded in-memory queue (best-effort buffering of short MQTT outages, not persistence)
- Validated YAML configuration
- Structured `key=value` logging
- Docker Compose demo
- Unit, integration and end-to-end tests
- GitHub Actions build/test workflow

Not included in MVP 1:

- Modbus TCP
- Store-and-forward
- SQLite persistence
- Sparkplug B
- ISA-95 hierarchy beyond basic asset identifiers
- TLS/mTLS
- Grafana / InfluxDB / Prometheus

See [Roadmap](#roadmap).

## Example telemetry

Captured from the demo (topic `industrial/site01/line01/oven01/temperature`):

```json
{
  "asset": "site01.line01.oven01",
  "tag": "temperature",
  "value": 648.9,
  "unit": "degC",
  "quality": "GOOD",
  "source_timestamp": "2026-10-18T01:42:53.585Z",
  "edge_timestamp": "2026-10-18T01:42:54.395Z",
  "protocol": "opcua"
}
```

`source_timestamp` comes from the OPC UA server; `edge_timestamp` is taken when the gateway
receives the update. `quality` is `GOOD`, `UNCERTAIN` or `BAD`, passed through unchanged.

## Quick start

Requires Docker with the Compose plugin. Run from the repository root:

```bash
docker compose up --build -d --wait
docker compose exec mosquitto mosquitto_sub -t 'industrial/#' -v
```

The first build compiles the gateway and its dependencies and takes several minutes; later
builds reuse the build cache. The subscriber runs inside the broker container, so no MQTT client
is needed on the host (with `mosquitto-clients` installed you can also run
`mosquitto_sub -h localhost -p 1883 -t 'industrial/#' -v`).

Example output, captured from the demo:

```text
industrial/site01/line01/press01/pressure {"asset":"site01.line01.press01","tag":"pressure","value":179.64,"unit":"bar","quality":"GOOD","source_timestamp":"2026-10-18T01:42:53.585Z","edge_timestamp":"2026-10-18T01:42:54.395Z","protocol":"opcua"}
industrial/site01/line01/oven01/temperature {"asset":"site01.line01.oven01","tag":"temperature","value":648.9,"unit":"degC","quality":"GOOD","source_timestamp":"2026-10-18T01:42:53.585Z","edge_timestamp":"2026-10-18T01:42:54.395Z","protocol":"opcua"}
industrial/site01/line01/press01/production_count {"asset":"site01.line01.press01","tag":"production_count","value":18,"unit":"units","quality":"GOOD","source_timestamp":"2026-10-18T01:42:55.086Z","edge_timestamp":"2026-10-18T01:42:55.395Z","protocol":"opcua"}
```

Values change frequently for pressure, temperature and cycle time; production counters and
machine states only change when the simulated equipment counts a part or changes state (see
[docs/RUNBOOK.md](docs/RUNBOOK.md#5-verify-mqtt-output)).

```bash
docker compose logs -f edge-gateway   # gateway logs
docker compose down                   # stop and remove the demo
```

The Compose stack is a reproducible reference/demo environment: anonymous MQTT, no TLS, no
persistence, ports published on 127.0.0.1 only. It is not a hardened production deployment.
See [docs/RUNBOOK.md](docs/RUNBOOK.md) for verification, failure tests and troubleshooting.

## Testing

GitHub Actions builds with GCC and Clang (Debug and Release, warnings as errors) and runs:

- unit tests;
- integration tests against the real Python simulator and a real Mosquitto broker;
- end-to-end tests of the gateway executable;
- the Compose demo smoke test, which also restarts Mosquitto and the simulator and checks that
  the running gateway reconnects and resumes publishing;
- clang-tidy, clang-format and the simulator's own tests.

## Roadmap

Planned, not implemented in v0.1.0: Modbus TCP (MVP 2), SQLite store-and-forward (MVP 3),
metrics and observability (MVP 4), ISA-95 asset modeling, TLS/mTLS and Sparkplug B. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#8-planned-evolution-not-implemented).

## Engineering goals

- Modern C++20
- Clear ownership and lifecycle
- Protocol adapters isolated from the domain model
- Reproducible local environment
- Testable behavior
- Industrially credible telemetry semantics
- Documentation of architecture decisions

See:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/RUNBOOK.md](docs/RUNBOOK.md) (demo operation, smoke test, troubleshooting)
- [docs/ISSUES.md](docs/ISSUES.md) (MVP 1 backlog and status)
- [docs/adr/](docs/adr/) (architecture decision records)
- [CHANGELOG.md](CHANGELOG.md)
- [CONTRIBUTING.md](CONTRIBUTING.md)

## License

Copyright 2026 Ernesto Guevara. Licensed under the Apache License 2.0; see [LICENSE](LICENSE).
Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
