# Architecture — MVP 1

## 1. Scope

MVP 1 implements the minimum complete OT-to-IT data path:

```text
Python OPC UA Simulator
        |
        | OPC UA subscriptions
        v
Open62541Client
        |
        | DataChange (protocol-neutral)
        v
Gateway Runtime  (mapping, edge timestamp, bounded queue)
        |
        | TelemetryPoint
        v
MqttPublisher
        |
        | MQTT 5 / JSON
        v
Mosquitto
```

The architecture deliberately separates protocol-specific code from the internal telemetry model:
the runtime depends only on the `IOpcUaClient` and `ITelemetryPublisher` interfaces, and no
open62541 or Paho type appears in a public header.

## 2. Components

### 2.1 Plant Simulator

Responsibilities:

- Run an OPC UA server (Python, asyncua)
- Expose a small but realistic plant model
- Generate deterministic industrial-like data
- Preserve state between scan intervals
- Provide source timestamps and OPC UA quality

Simulated assets:

```text
Site01
└── Line01
    ├── Press01
    │   ├── State
    │   ├── Pressure
    │   ├── CycleTime
    │   └── ProductionCount
    └── Oven01
        ├── State
        ├── Temperature
        └── ProductionCount
```

Values are bounded and stateful, not random: the model advances in fixed simulated time steps,
all randomness is seeded, and machine states follow a scripted, looping operating scenario that
includes scripted Bad/Uncertain quality windows. A variable is written (with a new source
timestamp) only when its value or status changes. See `simulator/README.md`.

### 2.2 OPC UA Adapter

Implementation: `Open62541Client` (open62541 1.4.21) behind `IOpcUaClient`.

Responsibilities:

- Connect/disconnect from the configured OPC UA endpoint
- Create the subscription and the monitored items
- Convert OPC UA notifications into protocol-neutral `DataChange` updates
- Preserve the source timestamp and the raw OPC UA StatusCode
- Map the StatusCode severity to `DataQuality` (GOOD/UNCERTAIN/BAD)
- Reconnect after connection failures

One adapter-owned worker thread exclusively owns the open62541 client. Every connection attempt
uses a fresh client, so a reconnect always recreates the session, the subscription and all
monitored items. Reconnects use bounded exponential backoff (500 ms up to 30 s by default).
Values that cannot be represented (arrays, unsupported types, non-finite floats) are reported as
BAD without a value and logged.

### 2.3 Domain Model

Primary type:

```cpp
struct TelemetryPoint {
    std::string asset;
    std::string tag;
    TelemetryValue value;
    std::string unit;
    DataQuality quality;
    std::optional<Timestamp> source_timestamp; // absent if the source sent none
    Timestamp edge_timestamp;
    std::string protocol;
};
```

`TelemetryValue` supports:

- boolean
- signed integer
- unsigned integer
- floating point
- string

`DataQuality`:

```text
GOOD
UNCERTAIN
BAD
```

### 2.4 Gateway Core

Implementation: `GatewayRuntime`.

Responsibilities:

- Own the lifecycle of the OPC UA client and the publisher
- Receive normalized updates
- Apply asset/tag/unit metadata from the configuration
- Add the edge timestamp (taken when the update is received, before it is queued)
- Forward telemetry to the publisher
- Log failures and state transitions

The OPC UA data callback only maps, timestamps and pushes into a bounded in-memory queue; it never
performs network I/O or logging. A single consumer thread takes points from the queue and
publishes them. An update that carries no value is not turned into a `TelemetryPoint`; it is
counted per item together with its most recent status code. Quality is never normalized:
UNCERTAIN and BAD values are published as such.

The queue holds up to `runtime.queue_capacity` telemetry points (default 10000) and drops the
oldest point when full. While the MQTT connection is down the consumer stops taking points, so
the queue provides best-effort, in-memory buffering of the most recent points during short
outages. It is not persistent store-and-forward: queued points are lost on restart and failed
publishes are not retried in MVP 1.

### 2.5 MQTT Publisher

Implementation: `MqttPublisher` (Eclipse Paho MQTT C 1.3.16, MQTTAsync API) behind
`ITelemetryPublisher`.

Responsibilities:

- Connect to Mosquitto using MQTT 5
- Serialize `TelemetryPoint` as JSON
- Publish to deterministic topics
- Reconnect after broker interruptions
- Report each publish outcome to the caller

Topic convention:

```text
<topic_prefix>/<asset levels>/<tag>
```

The asset identifier is split on `.`. Example with `topic_prefix: industrial`:

```text
industrial/site01/line01/oven01/temperature
```

QoS 1 and retain false are fixed in MVP 1. A publish is reported as delivered only after a
successful PUBACK; MQTT 5 PUBACK reason codes are mapped to retry dispositions (retryable or
permanent). While disconnected, a publish returns immediately without being sent. The session
uses clean start with session expiry 0; reconnects use bounded exponential backoff.

### 2.6 Configuration

Configuration is externalized in YAML (see `configs/gateway.example.yaml`).

Example:

```yaml
opcua:
  endpoint: opc.tcp://plant-simulator:4840
  subscription_interval_ms: 1000

mqtt:
  host: mosquitto
  port: 1883
  qos: 1
  topic_prefix: industrial
  client_id: ieg-gateway-01        # optional; development fallback if omitted

runtime:                           # optional section
  queue_capacity: 10000            # telemetry points
  shutdown_drain_timeout_ms: 3000

assets:
  - asset: site01.line01.press01
    nodes:
      - node_id: "ns=2;s=Line01.Press01.Pressure"
        tag: pressure
        unit: bar
      - node_id: "ns=2;s=Line01.Press01.ProductionCount"
        tag: production_count
        unit: units
```

Invalid configuration causes a clear startup failure listing every problem (exit code 2). The
loader accepts MQTT QoS 0–2; the MVP 1 runtime rejects anything other than QoS 1 at startup.

## 3. Threading Model

Implemented model:

```text
OPC UA worker thread (open62541 client)
        |  data callback: map + edge timestamp + try_push (no I/O)
        v
bounded telemetry queue
        |
        v
single publishing (consumer) thread  --->  MqttPublisher (Paho)
```

Additional threads: the MQTT publisher's connection worker, Paho's internal threads, a monitor
thread that performs all connection-transition and summary logging outside the adapter
callbacks, and the main thread, which waits for SIGINT/SIGTERM with `sigwait`.

Rationale:

- Keep OPC UA callback latency low
- Avoid MQTT network operations inside OPC UA callbacks
- Establish an architecture that can evolve toward store-and-forward
- Make queue depth observable (reported in the periodic summary log)

No thread-per-tag design.

Shutdown is bounded and ordered: stop the OPC UA client (producers) → close the queue → let the
consumer drain until it finishes or the drain timeout passes (then the publisher is stopped to
interrupt a blocked publish) → stop the publisher → join all threads.

## 4. Error Handling

Errors are grouped into:

### Configuration errors
Fatal at startup (exit code 2).

Examples:

- malformed YAML
- duplicated asset/tag
- unsupported value (e.g. MQTT QoS other than 1 in MVP 1)
- missing OPC UA endpoint

### Connectivity errors
Non-fatal at startup and afterwards.

Examples:

- OPC UA server unavailable
- MQTT broker unavailable

Both adapters retry with bounded exponential backoff and report their connection state.

### Data errors
Logged and counted; processing continues.

Examples:

- unexpected OPC UA type (reported as BAD without a value)
- invalid data quality (reserved StatusCode severity is treated as BAD)
- serialization failure (the point is not published)

Exit codes: `0` after a requested shutdown, `1` start failure, `2` configuration error.

## 5. Logging

Logging is structured and machine-readable: each gateway line is
`<UTC timestamp> <level> [<component>] event=<name> key=value…`, with components `gateway`,
`runtime`, `opcua` and `mqtt` and fields such as `endpoint`/`broker`, `asset`, `tag`, `status`,
`reason` and `error` where applicable. The simulator logs JSON lines.

Individual telemetry samples are not logged at INFO level; the runtime logs connection
transitions and a periodic summary (delivered, dropped and failed counts, queue depth).

## 6. Testing Strategy

### Unit tests

- Telemetry value conversion and JSON serialization
- OPC UA quality mapping and DataValue conversion
- Topic construction and MQTT reason-code mapping
- Configuration validation
- Bounded queue, publisher and runtime behavior with mocks, including concurrency and shutdown

### Integration tests

- `Open62541Client` against the real Python simulator, including reconnect after a simulator
  restart
- `MqttPublisher` against a real Mosquitto broker, including broker restart, timeouts and
  shutdown while a publish is blocked

### End-to-end tests

The gateway executable runs between the real simulator and a real broker. The tests assert
that:

1. The gateway connects to OPC UA.
2. The gateway subscribes to the configured nodes.
3. MQTT messages appear on every configured topic.
4. Payloads have the required fields.
5. Asset/tag values are correct.
6. Source timestamps exist.
7. Edge timestamps exist.
8. Quality is valid.

The Compose smoke test (`scripts/compose-smoke-test.sh`) checks the same on the Docker Compose
demo, restarts Mosquitto and the simulator to verify that the running gateway reconnects and
resumes publishing, and verifies a clean SIGTERM shutdown.

## 7. Security

MVP 1 uses local anonymous OPC UA (security mode None) and unauthenticated MQTT for development
convenience. The Compose demo publishes its ports on 127.0.0.1 only.

This is explicitly not a production security configuration.

TLS, certificates and mTLS are planned for a later milestone.

## 8. Planned Evolution (not implemented)

```text
MVP 1  OPC UA -> MQTT                      (v0.1.0, implemented)
MVP 2  Modbus + common acquisition model
MVP 3  SQLite store-and-forward
MVP 4  metrics / observability
v1.0   ISA-95 asset modeling + failure scenarios
v1.1   TLS/mTLS
v1.2   Sparkplug B
v1.3   benchmarks / performance
```
