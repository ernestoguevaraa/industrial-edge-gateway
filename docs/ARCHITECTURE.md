# Architecture — MVP 1 + MVP 2 (OPC UA and Modbus TCP)

## 1. Scope

MVP 1 implements the minimum complete OT-to-IT data path; MVP 2 adds Modbus TCP devices as
further acquisition sources of the same pipeline:

```text
OPC UA server                         Modbus TCP device(s)
        |                                     |
        | OPC UA subscriptions                | polled reads (FC01-FC04)
        v                                     v
Open62541Client  (IOpcUaClient)       LibModbusClient (IModbusClient) + decoder
        |                                     |
        v                                     v
OpcUaSource "opcua-main"              ModbusTcpSource "<source_id>" (one per device)
        |                                     |
        +---- acquisition::Update (protocol-neutral, keyed by source_id + item_id)
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
the runtime depends only on the protocol-neutral `IAcquisitionSource` and `ITelemetryPublisher`
interfaces, and no open62541, OPC UA client or Paho type appears in its public header. The OPC UA
client is wrapped by the `OpcUaSource` bridge and each Modbus device by a `ModbusTcpSource`;
`make_gateway_runtime()` builds one source per configured source (OPC UA-only, Modbus-only and
mixed deployments).

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

### 2.3 Acquisition API and OPC UA bridge

`ieg::acquisition` (`gateway/include/ieg/acquisition/`) is the SDK-neutral ingress shared by all
protocol adapters:

- `Item { id, locator }`: what the runtime requests. Item IDs are unique within a source; the
  runtime keys items by `(source_id, item_id)`.
- `Update { source_id, item_id, optional value, quality, optional source_timestamp,
  optional native_status, diagnostic }`. `native_status` is the raw status in the protocol's own
  code space (the OPC UA StatusCode, a Modbus exception code). It is recorded and logged for
  diagnostics but never interpreted, and never manufactured for local failures; `diagnostic` is a
  stable symbolic reason for those (e.g. `timeout`, `decode_non_finite_value`). `quality` is the
  only protocol-independent quality.
- `Status { source_id, state, detail }` with Connecting/Connected/Disconnected.
- `IAcquisitionSource { id(), start(items, callbacks), stop() }`. Callbacks run serialized on
  source-owned threads and must not block. `start()` is called at most once (an invalid request
  does not consume it); `stop()` is idempotent, safe before `start()` and after a `start()` that
  threw, and returns only once no callback can run.

`OpcUaSource` adapts an `IOpcUaClient` to this contract. It forwards items as monitored items,
copies value, quality and source timestamp unchanged, forwards the raw StatusCode as
`native_status` (so `BadNodeIdUnknown` stays `0x80340000`, and a local conversion failure keeps the
server's Good code while quality is Bad), and maps connection states one to one. Reconnection
and callback quiescence are provided by `Open62541Client`. If the client's `start()` fails
unexpectedly the bridge stops the client at once; a rejected request leaves it startable.

The `opcua` section configures at most one OPC UA source, with source ID `opcua-main`; each
`modbus.devices` entry is one `ModbusTcpSource` with its own `source_id` (see
`docs/MVP2_MODBUS_DESIGN.md`).

### 2.4 Domain Model

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

### 2.5 Gateway Core

Implementation: `GatewayRuntime`.

Responsibilities:

- Own the lifecycle of the acquisition sources and the publisher
- Receive normalized `acquisition::Update`s
- Apply asset/tag/unit metadata from the configuration
- Add the edge timestamp (taken when the update is received, before it is queued)
- Forward telemetry to the publisher
- Log failures and state transitions

The source data callback only maps, timestamps and pushes into a bounded in-memory queue; it never
performs network I/O or logging. A single consumer thread takes points from the queue and
publishes them. An update that carries no value is not turned into a `TelemetryPoint`; it is
counted per item together with its most recent native status and diagnostic
(`event=no_value_updates ... last_status=0x80340000`). Items are keyed by `(source_id, item_id)`:
item IDs are unique per source only (OPC UA keeps its MVP 1 configuration-order IDs), lookups go
through an immutable table (hash of the source ID, then an index), and updates from an unknown
source (`unknown_source`) or for an item a source was not asked for (`unknown_item`) are counted
and never published. Quality is never normalized: UNCERTAIN and BAD values are published as such.
`TelemetryPoint::protocol` comes from the item binding (`opcua` or `modbus_tcp`); Modbus points
have no source timestamp (`"source_timestamp": null`).

Callbacks of different sources run concurrently on their own threads; the data path uses only
the immutable item table, atomic counters, a per-item lock for the last diagnostic (never shared
between sources) and the queue's short critical section, so one source never blocks another.
Connection states are logged per source: `event=connection_state component=<protocol>
source_id=<id> target=<endpoint> state=...` (the MVP 1 `component=opcua` and `component=mqtt`
lines keep their form).

The queue holds up to `runtime.queue_capacity` telemetry points (default 10000) and drops the
oldest point when full. While the MQTT connection is down the consumer stops taking points, so
the queue provides best-effort, in-memory buffering of the most recent points during short
outages. It is not persistent store-and-forward: queued points are lost on restart and failed
publishes are not retried in MVP 1.

### 2.6 MQTT Publisher

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

### 2.7 Configuration

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
loader accepts MQTT QoS 0–2; the runtime rejects anything other than QoS 1 at startup.

The loader also accepts the MVP 2 multi-source schema: `opcua` + `assets` become optional when at
least one Modbus TCP device is configured under `modbus.devices`, source IDs are unique, and
`(asset, tag)` is unique across all sources. The schema, its validation rules and the Modbus data
convention (zero-based offsets, big-endian bytes, explicit word order for 32-bit types) are
documented in `docs/MVP2_MODBUS_DESIGN.md`; `configs/gateway.mixed.example.yaml` is a mixed
example.

## 3. Threading Model

Implemented model:

```text
OPC UA worker thread          Modbus worker thread per device (ModbusTcpSource)
(open62541 -> OpcUaSource)    (LibModbusClient, sequential polling, decoder)
        |                             |
        |  source callbacks (concurrent across sources):
        |  map + edge timestamp + try_push (no I/O)
        v                             v
bounded telemetry queue
        |
        v
single publishing (consumer) thread  --->  MqttPublisher (Paho)
```

Each Modbus device has one worker thread (`ModbusTcpSource`) that owns its client, polls
sequentially and invokes that source's callbacks; the OPC UA client has its own worker thread.

Additional threads: the MQTT publisher's connection worker, Paho's internal threads, a monitor
thread that performs all connection-transition and summary logging outside the adapter
callbacks, and the main thread, which waits for SIGINT/SIGTERM with `sigwait`.

Rationale:

- Keep OPC UA callback latency low
- Avoid MQTT network operations inside OPC UA callbacks
- Establish an architecture that can evolve toward store-and-forward
- Make queue depth observable (reported in the periodic summary log)

No thread-per-tag design.

Shutdown is bounded and ordered: stop all acquisition sources (producers, concurrently, so the
slowest source bounds the wait; each closes its own client on its own thread) → close the queue →
let the consumer drain until it finishes or the drain timeout passes (then the publisher is
stopped to interrupt a blocked publish) → stop the publisher → join all threads. Start runs in the
reverse order (publisher and consumer before the sources, then the sources one by one); if any
source's `start()` throws, every source whose start was attempted is stopped before the queue is
closed and the publisher stopped. A Modbus source may need up to one request timeout
(`timeout_ms`) to stop, or one connect attempt (plus unbounded DNS resolution for host names).

## 4. Error Handling

Errors are grouped into:

### Configuration errors
Fatal at startup (exit code 2).

Examples:

- malformed YAML
- duplicated asset/tag
- unsupported value (e.g. MQTT QoS other than 1 in MVP 1)
- missing OPC UA endpoint
- no acquisition source configured

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
- The acquisition contract on `OpcUaSource` (mapping, native status, lifecycle and start
  failures), and the runtime wired to the real `OpcUaSource` over a mocked OPC UA client
- Multi-source configuration (Modbus schema validation) and the transport-independent Modbus data
  decoder (`ieg::modbus::decode`)
- The multi-source runtime with mock sources: routing by `(source_id, item_id)`, unknown sources,
  partial start failures, concurrent shutdown, concurrent callbacks, per-item diagnostics
- The Modbus TCP client (`LibModbusClient`, libmodbus 3.1.10) against a deterministic in-process
  Modbus TCP server, and a scripted `FakeModbusClient` for future poller tests
- The Modbus TCP acquisition source (`ModbusTcpSource`: read grouping, polling schedule,
  reconnection, shutdown) with a fake device and a manual clock, and against the in-process
  server

### Integration tests

- `Open62541Client` against the real Python simulator, including reconnect after a simulator
  restart
- `OpcUaSource` over `Open62541Client` against the simulator: values, source timestamps, raw
  StatusCodes (including `BadNodeIdUnknown`) and a final, silent `stop()`
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

With a deterministic in-process Modbus TCP server next to the simulator, further tests run a
mixed OPC UA + Modbus configuration (unchanged OPC UA topics and payloads, Modbus payloads with
`"protocol": "modbus_tcp"` and `"source_timestamp": null`, recovery after a Modbus outage in the
same gateway process) and a Modbus-only configuration.

The Compose smoke test (`scripts/compose-smoke-test.sh`) checks the same on the Docker Compose
demo, restarts Mosquitto and the simulator to verify that the running gateway reconnects and
resumes publishing, and verifies a clean SIGTERM shutdown.

The MVP 2 smoke test (`scripts/compose-mvp2-smoke-test.sh`) runs `compose.mvp2.yaml`: the OPC UA
simulator, the deterministic Modbus TCP simulator (`modbus-simulator/`, its own image) and the
gateway with the mixed configuration. It checks the exact topic set of both sources, protocols,
Modbus values, scales and JSON types (`"source_timestamp": null`), that restarting only the
Modbus simulator does not interrupt OPC UA publishing, recovery of both protocols after a
Mosquitto restart, and a clean SIGTERM shutdown.

## 7. Security

MVP 1 uses local anonymous OPC UA (security mode None) and unauthenticated MQTT for development
convenience. Modbus TCP has no security at all (plaintext, no authentication); use it only on
isolated networks. The MVP 2 demo (`compose.mvp2.yaml`, Modbus simulator) inherits all of this and
is likewise not a production security configuration.

libmodbus 3.1.10 (the Ubuntu 24.04 package) has open CVEs (no fixed release is known for
CVE-2023-26793; CVE-2024-10918 is fixed upstream in 3.1.11). The
gateway imports only libmodbus client functions (connect/close, the four read functions and
settings), so the server-side CVE-2024-10918 (`modbus_reply`) and CVE-2024-34244
(`modbus_write_bits`) are not reachable. CVE-2023-26793 (heap overflow reported in
`read_io_status`, used by `modbus_read_bits`/`modbus_read_input_bits`) is on the read path of
coils and discrete inputs: a malicious or compromised Modbus device is the attacker model. The
gateway validates request quantities and passes exactly-sized buffers, and a fuzzing run against
an AddressSanitizer build of libmodbus 3.1.10 with malformed responses found no memory error, but
that is not proof. Connect the gateway only to trusted devices on isolated networks. The Compose demo publishes its ports on 127.0.0.1 only.

This is explicitly not a production security configuration.

TLS, certificates and mTLS are planned for a later milestone.

## 8. Planned Evolution (not implemented)

```text
MVP 1  OPC UA -> MQTT                      (v0.1.0, implemented)
MVP 2  Modbus + common acquisition model   (implemented, unreleased: acquisition API, config,
                                            decoder, TCP client, polling, multi-source runtime)
MVP 3  SQLite store-and-forward
MVP 4  metrics / observability
v1.0   ISA-95 asset modeling + failure scenarios
v1.1   TLS/mTLS
v1.2   Sparkplug B
v1.3   benchmarks / performance
```
