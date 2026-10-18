# Runbook — Industrial Edge Gateway MVP 1

## 1. Purpose

This runbook describes how to build, start, verify and troubleshoot the MVP 1 environment.

MVP 1 data path:

```text
OPC UA Plant Simulator -> C++20 Gateway -> MQTT Broker
```

The Docker Compose stack (`compose.yaml`) is a reproducible reference/demo environment:
anonymous MQTT, no TLS, no persistence, host ports published on `127.0.0.1` only. It is not a
hardened production deployment.

## 2. Prerequisites

To run the demo:

- Docker with the Compose plugin (`docker compose`)

To build and test the gateway locally (section 7):

- Linux or WSL2, CMake ≥ 3.25, GCC or Clang with C++20, Ninja (optional)
- `libmbedtls-dev` (required by the official open62541 amalgamation)
- Python 3.12 (simulator) and `mosquitto` (broker binary) for the integration tests

Optional local debugging tools:

- `mosquitto_sub` / `mosquitto_pub` (`mosquitto-clients`)
- an OPC UA browser such as UaExpert

## 3. Clone

```bash
git clone <repository-url>
cd industrial-edge-gateway
```

## 4. Start Complete Demo

In the background, returning once the services are up:

```bash
docker compose up --build -d --wait
```

Or in the foreground with interleaved logs (stop with `Ctrl+C`):

```bash
docker compose up --build
```

The first build compiles the gateway and its dependencies and takes several minutes; later
builds reuse the build cache.

Services:

| Service | Image | Host port | Purpose |
|---|---|---|---|
| `plant-simulator` | `ieg/plant-simulator:mvp1` (built from `simulator/`) | `127.0.0.1:4840` | OPC UA plant simulator |
| `mosquitto` | `eclipse-mosquitto:2.0.22` | `127.0.0.1:1883` | MQTT broker (`docker/mosquitto/mosquitto.conf`) |
| `edge-gateway` | `ieg/edge-gateway:mvp1` (built from `docker/gateway/Dockerfile`) | none | the C++20 gateway, Release build |

Containers reach each other by service name (`opc.tcp://plant-simulator:4840`,
`mosquitto:1883`). The gateway uses `configs/gateway.example.yaml`, mounted read-only.

Startup order: the gateway waits until the simulator and the broker report healthy. This only
avoids connection-failure noise at startup; the gateway reconnects to both on its own (see
sections 10 and 11). If a host port is taken, see section 13.

## 5. Verify MQTT Output

Subscribe with the client inside the broker container (no host tools needed):

```bash
docker compose exec mosquitto mosquitto_sub -t 'industrial/#' -v
```

Or from the host, with `mosquitto-clients` installed:

```bash
mosquitto_sub -h localhost -p 1883 -t 'industrial/#' -v
```

Topics follow `industrial/<site>/<line>/<asset>/<tag>`; there are seven:

```text
industrial/site01/line01/press01/state
industrial/site01/line01/press01/pressure
industrial/site01/line01/press01/cycle_time
industrial/site01/line01/press01/production_count
industrial/site01/line01/oven01/state
industrial/site01/line01/oven01/temperature
industrial/site01/line01/oven01/production_count
```

Example output, captured from the demo:

```text
industrial/site01/line01/press01/state {"asset":"site01.line01.press01","tag":"state","value":"STARTING","unit":"","quality":"GOOD","source_timestamp":"2026-10-18T01:41:33.543Z","edge_timestamp":"2026-10-18T01:41:35.354Z","protocol":"opcua"}
industrial/site01/line01/press01/pressure {"asset":"site01.line01.press01","tag":"pressure","value":179.64,"unit":"bar","quality":"GOOD","source_timestamp":"2026-10-18T01:42:53.585Z","edge_timestamp":"2026-10-18T01:42:54.395Z","protocol":"opcua"}
industrial/site01/line01/press01/cycle_time {"asset":"site01.line01.press01","tag":"cycle_time","value":3.935,"unit":"s","quality":"GOOD","source_timestamp":"2026-10-18T01:42:55.086Z","edge_timestamp":"2026-10-18T01:42:55.395Z","protocol":"opcua"}
industrial/site01/line01/oven01/temperature {"asset":"site01.line01.oven01","tag":"temperature","value":648.9,"unit":"degC","quality":"GOOD","source_timestamp":"2026-10-18T01:42:53.585Z","edge_timestamp":"2026-10-18T01:42:54.395Z","protocol":"opcua"}
```

Which topics a subscriber sees depends on when it subscribes. Messages are not retained, and
the simulator only writes a value when it changes:

- When the gateway connects it publishes the current value of all seven tags once. A subscriber
  started immediately after `docker compose up --build -d --wait` usually catches these initial
  values (observed: all seven topics within the first second).
- A subscriber started later sees pressure, temperature and cycle time continuously (about
  every 0.5 s while they change), production counters only when a part is counted, and machine
  states only when the scripted operating scenario changes state (phases last between 5 s and
  2 minutes; see `simulator/README.md`). Observed in a 20 s window about one minute after startup: oven temperature, press
  pressure, cycle time and production count; no state messages, and no oven production count
  while the oven was still heating up.

To see the current value of every tag again, keep a subscriber running and restart the gateway:

```bash
docker compose restart edge-gateway
```

During the simulator's scripted fault windows some updates carry `"quality":"BAD"` or
`"quality":"UNCERTAIN"`; quality is passed through unchanged (see `simulator/README.md`).

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
  "value": 648.9,
  "unit": "degC",
  "quality": "GOOD",
  "source_timestamp": "2026-10-18T01:42:53.585Z",
  "edge_timestamp": "2026-10-18T01:42:54.395Z",
  "protocol": "opcua"
}
```

`source_timestamp` is the simulator's timestamp; `edge_timestamp` is taken when the gateway
receives the update. MQTT messages use QoS 1 and retain=false.

## 7. Build Gateway Locally

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build -j
```

Run the gateway against a configuration with real endpoints (the example configuration uses the
Compose service names):

```bash
./build/gateway/gateway configs/gateway.example.yaml
```

The gateway image (`ieg/edge-gateway:mvp1`) contains the same executable, built in Release mode.
Its default configuration `/etc/ieg/gateway.yaml` is the Compose/demo configuration; a standalone
container needs a configuration with its real OPC UA and MQTT endpoints, for example:

```bash
docker run --rm -v "$PWD/my-gateway.yaml:/etc/ieg/gateway.yaml:ro" ieg/edge-gateway:mvp1
```

## 8. Run Unit Tests

```bash
ctest --test-dir build --output-on-failure
```

The integration tests run when `IEG_SIMULATOR_PYTHON` (a Python with
`simulator/requirements.txt` installed) and `IEG_MOSQUITTO` (path to the `mosquitto` binary) are
set; otherwise they are skipped.

## 9. Validate OPC UA Simulator

From the host, the simulator is reachable at:

```text
opc.tcp://localhost:4840
```

Inside the Compose network the gateway uses:

```text
opc.tcp://plant-simulator:4840
```

Optional verification can be performed with an OPC UA browser (security mode None, anonymous).

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

Stop the simulator, watch the gateway, then start it again:

```bash
docker compose stop plant-simulator
docker compose logs -f edge-gateway
docker compose start plant-simulator
```

Expected gateway behavior (observed log events, abbreviated):

```text
warning [opcua] event=connection_lost ... reason="secure channel closed" retry_in_ms=500
warning [runtime] event=connection_state component=opcua ... state=Disconnected ...
warning [opcua] event=connect_failed ... reason="BadConnectionRejected" retry_in_ms=1000
info    [opcua] event=connected ... items=7 failed_items=0
info    [runtime] event=connection_state component=opcua ... state=Connected ...
```

The gateway keeps running, retries with exponential backoff (up to 30 s), recreates the session,
subscription and all monitored items after reconnecting, and resumes publishing.

`docker compose restart plant-simulator` exercises the same path.

## 11. Failure Test — MQTT Broker

```bash
docker compose stop mosquitto
docker compose logs -f edge-gateway
docker compose start mosquitto
```

Expected gateway behavior (observed log events, abbreviated):

```text
warning [mqtt] event=connection_lost broker=tcp://mosquitto:1883 reason="connection lost" retry_in_ms=500
warning [runtime] event=connection_state component=mqtt ... state=Disconnected ...
warning [mqtt] event=connect_failed ... retry_in_ms=1000
info    [mqtt] event=connected broker=tcp://mosquitto:1883 client_id=ieg-gateway-01
info    [runtime] event=connection_state component=mqtt ... state=Connected ...
```

While the broker is unreachable the gateway stops taking points from its in-memory queue, so the
most recent points (up to `runtime.queue_capacity`, default 10000 telemetry points; oldest
dropped first) are published after the reconnect. How long that bridges depends on the telemetry
rate. This is best-effort only:

- MVP 1 does **not** guarantee telemetry preservation during MQTT outages.
- Queued points are lost if the gateway restarts; a point whose publish fails is not retried.
- Persistent store-and-forward is planned for MVP 3.

The `event=telemetry_summary` log line reports delivered, dropped and failed counts.

## 12. Logs

Show all services:

```bash
docker compose logs -f
```

Gateway only:

```bash
docker compose logs -f edge-gateway
```

Simulator only (JSON lines):

```bash
docker compose logs -f plant-simulator
```

Mosquitto only:

```bash
docker compose logs -f mosquitto
```

Gateway log lines have the form `<UTC timestamp> <level> [<component>] event=<name> key=value…`.
Set `IEG_LOG_LEVEL` (`trace`, `debug`, `info`, `warn`, `error`) in `compose.yaml` to change the
level. Expected gateway lines after startup:

```text
info [mqtt] event=starting broker=tcp://mosquitto:1883 client_id=ieg-gateway-01
info [opcua] event=starting endpoint=opc.tcp://plant-simulator:4840 items=7
info [runtime] event=started opcua_endpoint=opc.tcp://plant-simulator:4840 mqtt_broker=mosquitto:1883 items=7 queue_capacity_points=10000 drain_timeout_ms=3000
info [mqtt] event=connected broker=tcp://mosquitto:1883 client_id=ieg-gateway-01
info [opcua] event=connected endpoint=opc.tcp://plant-simulator:4840 publishing_interval_ms=1000 items=7 failed_items=0
```

Expected warning: `warning [opcua] event=open62541_log message="skip verifying ApplicationURI for
the SecurityPolicy ...#None"` appears with the unsecured MVP 1 connection and does not indicate a
problem. The MQTT library's startup banner is logged at debug level
(`debug [mqtt] event=paho_trace`), visible only with `IEG_LOG_LEVEL=debug`.

## 13. Common Problems

### Host port already in use

`docker compose up` fails with "address already in use" if port 1883 or 4840 is taken on the
host (for example by a system Mosquitto service). Either stop that service or choose other host
ports:

```bash
IEG_MQTT_HOST_PORT=11883 IEG_OPCUA_HOST_PORT=14840 docker compose up --build -d --wait
```

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
docker compose ps
docker compose logs edge-gateway
docker compose logs mosquitto
```

Then subscribe to everything:

```bash
docker compose exec mosquitto mosquitto_sub -t '#' -v
```

Remember that state and counter topics change rarely (section 5).

### Invalid NodeId

Compare `configs/gateway.example.yaml` against the simulator node definitions. A node that does
not exist on the server is reported in the gateway log (`event=item_create_failed`) and in the
periodic `event=no_value_updates` line with its NodeId and status code.

### Gateway exits at startup

```bash
docker compose ps -a
docker compose logs edge-gateway
```

Exit code 2 means the configuration was rejected; every problem is logged
(`event=config_error` or `event=startup_validation_failed`). Likely causes:

- invalid YAML
- missing mandatory configuration
- duplicate asset/tag definition
- unsupported configuration value (e.g. `mqtt.qos` other than 1 in MVP 1)

Exit code 1 means a component failed to start (`event=start_failed`). Configuration failures are
intentionally fatal.

## 14. Shutdown

Stop and remove the demo (containers and network):

```bash
docker compose down
```

Also remove the images used by the demo (the two locally built `ieg/*` images and the pulled
`eclipse-mosquitto` image):

```bash
docker compose down --rmi all
```

`docker compose down`, `docker compose stop` and `Ctrl+C` send SIGTERM. The gateway stops
producers, drains its queue for up to `runtime.shutdown_drain_timeout_ms`, disconnects and exits
with code 0 (observed: about one second). Its last log lines are:

```text
info [gateway] event=shutdown_requested signal=SIGTERM
info [runtime] event=stopping
info [opcua] event=stopped endpoint=opc.tcp://plant-simulator:4840
info [mqtt] event=stopped broker=tcp://mosquitto:1883
info [runtime] event=stopped delivered=... received=... queue_dropped=0 no_value=0 stopped_in_flight=0 shutdown_dropped=0
```

Gateway exit codes: `0` requested shutdown, `1` start failure, `2` configuration error.

## 15. Compose Smoke Test

`scripts/compose-smoke-test.sh` verifies the demo end to end and runs in CI (`compose-demo` job).
It needs only Docker and Bash on the host:

```bash
scripts/compose-smoke-test.sh
```

It builds the images, starts the stack as a separate Compose project (`ieg-smoke`, random
loopback host ports, so it does not disturb a running demo), subscribes before the gateway
starts, and checks that:

- exactly the seven expected topics arrive;
- every payload parses as JSON (with the Python inside the simulator container) and contains
  the required fields with the expected asset, tag and unit;
- the gateway logs its start and both connections;
- the gateway container stops on SIGTERM with exit code 0 within the grace period.

It removes its containers and network when it finishes, also on failure.

## 16. Definition of Done

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
