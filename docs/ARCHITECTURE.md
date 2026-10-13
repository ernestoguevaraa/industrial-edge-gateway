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
        | TelemetryPoint
        v
Gateway Core
        |
        | TelemetryPoint
        v
MQTT Publisher
        |
        | MQTT 5 / JSON
        v
Mosquitto
```

The architecture deliberately separates protocol-specific code from the internal telemetry model.

## 2. Components

### 2.1 Plant Simulator

Responsibilities:

- Run an OPC UA server
- Expose a small but realistic plant model
- Generate deterministic industrial-like data
- Preserve state between scan intervals
- Provide source timestamps and OPC UA quality

Initial simulated assets:

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

The simulator should avoid purely random values. Temperature, pressure, state and counters should follow bounded and stateful behavior.

### 2.2 OPC UA Adapter

Implementation: `Open62541Client`

Responsibilities:

- Connect/disconnect from configured OPC UA endpoint
- Create subscriptions
- Create monitored items
- Convert OPC UA callbacks into protocol-neutral updates
- Preserve source timestamp
- Map OPC UA status/quality to internal `DataQuality`
- Reconnect after temporary connection failure

The rest of the application must not depend directly on `open62541` types.

### 2.3 Domain Model

Primary type:

```cpp
struct TelemetryPoint {
    std::string asset;
    std::string tag;
    TelemetryValue value;
    std::string unit;
    DataQuality quality;
    Timestamp source_timestamp;
    Timestamp edge_timestamp;
    std::string protocol;
};
```

`TelemetryValue` should support at least:

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

Responsibilities:

- Own lifecycle of configured protocol adapters
- Receive normalized telemetry
- Apply asset/tag metadata
- Add edge timestamp
- Forward telemetry to output publishers
- Log failures and state transitions

MVP 1 does not introduce a persistent queue.

A bounded in-memory queue may be introduced if required by the callback/publisher threading model, but it should not be used as a substitute for the planned store-and-forward layer.

### 2.5 MQTT Publisher

Responsibilities:

- Connect to Mosquitto using MQTT 5
- Serialize `TelemetryPoint` as JSON
- Publish to deterministic topics
- Reconnect after broker interruptions
- Log publish failures

Initial topic convention:

```text
industrial/<site>/<line>/<asset>/<tag>
```

Example:

```text
industrial/site01/line01/oven01/temperature
```

QoS for MVP 1: QoS 1.

Retain: false.

### 2.6 Configuration

Configuration must be externalized.

Example:

```yaml
opcua:
  endpoint: opc.tcp://plant-simulator:4840
  subscription_interval_ms: 1000

mqtt:
  host: mosquitto
  port: 1883
  qos: 1

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

Invalid configuration must cause a clear startup failure.

## 3. Threading Model

MVP 1 should remain simple.

Preferred model:

```text
open62541 event loop / callback
        |
        v
bounded telemetry queue
        |
        v
single MQTT publishing worker
```

Rationale:

- Keep OPC UA callback latency low
- Avoid MQTT network operations inside OPC UA callbacks
- Establish an architecture that can evolve toward store-and-forward
- Make queue depth observable later

No thread-per-tag design.

## 4. Error Handling

Errors are grouped into:

### Configuration errors
Fatal at startup.

Examples:

- malformed YAML
- duplicated asset/tag
- unsupported type
- missing OPC UA endpoint

### Connectivity errors
Non-fatal after successful startup.

Examples:

- OPC UA server unavailable
- MQTT broker unavailable

The gateway should retry with bounded/exponential backoff.

### Data errors
Log and continue when possible.

Examples:

- unexpected OPC UA type
- invalid data quality
- serialization failure

## 5. Logging

Use structured, machine-readable logging where practical.

Minimum fields:

```text
timestamp
level
component
event
asset
tag
endpoint/broker where applicable
error
```

Avoid logging every telemetry sample at INFO level.

## 6. Testing Strategy

### Unit tests

- Telemetry value conversion
- OPC UA quality mapping
- Topic construction
- JSON serialization
- Configuration validation

### Integration test

Start:

- simulator
- Mosquitto
- gateway

Assert that:

1. Gateway connects to OPC UA.
2. Gateway subscribes to configured nodes.
3. MQTT message appears.
4. Payload has required fields.
5. Asset/tag values are correct.
6. Source timestamp exists.
7. Edge timestamp exists.
8. Quality is valid.

## 7. Security

MVP 1 uses local anonymous OPC UA and unauthenticated MQTT for development convenience.

This is explicitly not a production security configuration.

TLS, certificates and mTLS are planned for a later milestone.

## 8. Planned Evolution

```text
MVP 1  OPC UA -> MQTT
MVP 2  Modbus + common acquisition model
MVP 3  SQLite store-and-forward
MVP 4  metrics / observability
v1.0   ISA-95 asset modeling + failure scenarios
v1.1   TLS/mTLS
v1.2   Sparkplug B
v1.3   benchmarks / performance
```
