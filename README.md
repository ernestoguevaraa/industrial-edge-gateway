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
- `docs/RUNBOOK.md` (demo operation, smoke test, troubleshooting)
- `docs/ISSUES.md`
- `docs/adr/`
