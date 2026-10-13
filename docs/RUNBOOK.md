# Runbook — Industrial Edge Gateway MVP 1

## 1. Purpose

This runbook describes how to build, start, verify and troubleshoot the MVP 1 environment.

MVP 1 data path:

```text
OPC UA Plant Simulator -> C++20 Gateway -> MQTT Broker
```

## 2. Prerequisites

Recommended development environment:

- Linux or WSL2
- Docker
- Docker Compose
- CMake
- GCC or Clang
- Git

Optional local debugging tools:

- `mosquitto_sub`
- `mosquitto_pub`
- OPC UA browser such as UaExpert

## 3. Clone

```bash
git clone <repository-url>
cd industrial-edge-gateway
```

## 4. Start Complete Demo

```bash
docker compose up --build
```

Expected services:

```text
plant-simulator
mosquitto
edge-gateway
```

## 5. Verify MQTT Output

In another terminal:

```bash
mosquitto_sub \
  -h localhost \
  -p 1883 \
  -t 'industrial/#' \
  -v
```

Expected messages:

```text
industrial/site01/line01/press01/pressure {...}
industrial/site01/line01/press01/production_count {...}
industrial/site01/line01/oven01/temperature {...}
```

## 6. Verify Payload

A valid payload must contain:

```text
asset
tag
value
unit
quality
source_timestamp
edge_timestamp
protocol
```

Example:

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

## 7. Build Gateway Locally

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build -j
```

## 8. Run Unit Tests

```bash
ctest --test-dir build --output-on-failure
```

## 9. Validate OPC UA Simulator

The expected endpoint is:

```text
opc.tcp://localhost:4840
```

When using Docker networking the gateway should use:

```text
opc.tcp://plant-simulator:4840
```

Optional verification can be performed with an OPC UA browser.

Expected nodes:

```text
Line01.Press01.State
Line01.Press01.Pressure
Line01.Press01.CycleTime
Line01.Press01.ProductionCount

Line01.Oven01.State
Line01.Oven01.Temperature
Line01.Oven01.ProductionCount
```

## 10. Failure Test — OPC UA Server

Stop the simulator:

```bash
docker compose stop plant-simulator
```

Expected gateway behavior:

- Detect lost OPC UA connection
- Log connection loss
- Continue running
- Retry connection

Restart:

```bash
docker compose start plant-simulator
```

Expected behavior:

- Reconnect automatically
- Restore monitored items/subscriptions
- Resume MQTT publication

## 11. Failure Test — MQTT Broker

Stop Mosquitto:

```bash
docker compose stop mosquitto
```

Expected MVP 1 behavior:

- Detect MQTT disconnect
- Log broker connection loss
- Continue running
- Retry MQTT connection

Important:

MVP 1 does **not** guarantee telemetry preservation during MQTT outages.

Persistent store-and-forward is planned for MVP 3.

Restart:

```bash
docker compose start mosquitto
```

Expected behavior:

- MQTT reconnects
- New telemetry begins publishing again

## 12. Logs

Show all services:

```bash
docker compose logs -f
```

Gateway only:

```bash
docker compose logs -f edge-gateway
```

Simulator only:

```bash
docker compose logs -f plant-simulator
```

Mosquitto only:

```bash
docker compose logs -f mosquitto
```

## 13. Common Problems

### OPC UA connection refused

Check:

```bash
docker compose ps
docker compose logs plant-simulator
```

Confirm gateway endpoint:

```text
opc.tcp://plant-simulator:4840
```

### No MQTT messages

Check:

```bash
docker compose logs edge-gateway
docker compose logs mosquitto
```

Then subscribe manually:

```bash
mosquitto_sub -h localhost -t '#' -v
```

### Invalid NodeId

Compare `configs/gateway.yaml` against simulator node definitions.

The gateway should report the exact NodeId that failed.

### Gateway exits at startup

Likely causes:

- invalid YAML
- missing mandatory configuration
- duplicate asset/tag definition
- unsupported configuration value

Configuration failures are intentionally fatal.

## 14. Shutdown

```bash
docker compose down
```

Remove local containers and anonymous volumes if required:

```bash
docker compose down -v
```

## 15. Definition of Done

MVP 1 is complete only when:

- `docker compose up --build` starts all services
- OPC UA subscriptions work
- normalized telemetry reaches MQTT
- `mosquitto_sub` shows expected messages
- OPC UA reconnect succeeds
- MQTT reconnect succeeds
- unit tests pass
- integration test passes
- GitHub Actions build is green
- README and architecture documentation match actual behavior
