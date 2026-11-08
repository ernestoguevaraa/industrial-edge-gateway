# MVP 2 — Modbus TCP acquisition design

Status: Proposed (implementation not started)  
Tracking: [Issue #1](https://github.com/ernestoguevaraa/industrial-edge-gateway/issues/1)

## Motivation

MVP 1 already has a protocol-independent `TelemetryPoint` and MQTT pipeline, but `GatewayRuntime` depends directly on `IOpcUaClient`, `opcua::DataChange`, OPC UA item identifiers and OPC UA mapping. Adding Modbus directly to that runtime would couple ingestion and orchestration to individual SDKs. Refactor the ingress contract before implementing Modbus.

## Target pipeline

```text
Open62541Client --> OPC UA bridge --+
                                     +--> common acquisition callback --> GatewayRuntime --> bounded queue --> MQTT
ModbusTcpPoller ---------------------+
```

Adapters do not perform MQTT I/O. Ingress callbacks map metadata, set `edge_timestamp` at receipt, and enqueue without blocking. The existing single publishing consumer and MQTT 5 semantics remain unchanged.

## Proposed common contract

Introduce an SDK-neutral `ieg::acquisition` API (names may be adjusted during implementation):
- `SourceId`: unique per configured source, e.g. `opcua-main` or `modbus-press01`.
- `ItemId`: unique within one source; the runtime uses the pair `(SourceId, ItemId)` as the lookup key.
- `AcquisitionUpdate`: source id, item id, `optional<TelemetryValue>`, `DataQuality`, optional source timestamp, and an adapter-owned diagnostic string/code as needed. Do **not** force Modbus into an OPC UA StatusCode field.
- `SourceStatus`: source id, Connecting/Connected/Disconnected and diagnostic detail.
- `IAcquisitionSource`: `start(callbacks)`, `stop() noexcept`; callbacks have serialized/non-blocking lifetime semantics documented by the API. If an adapter manages multiple devices, status is separately keyed by device/source identity.

Build an OPC UA acquisition bridge that preserves existing subscriptions, values, timestamps, quality, reconnection, and no-value diagnostics. Keep adapter SDK types out of common headers. Ensure shutdown stops **all** acquisition producers before closing the queue.

## Configuration

Preserve the v0.1.0 YAML schema for OPC UA-only deployments unchanged. Extend the loader with an optional Modbus configuration. Do not require `opcua` when a Modbus-only deployment is configured. Validate global uniqueness of `(asset, tag)` across all sources, plus unique source IDs.

Illustrative proposal (not yet accepted by the loader):

```yaml
mqtt:
  host: mosquitto
  port: 1883
  qos: 1
  topic_prefix: industrial

modbus:
  devices:
    - id: press-plc
      host: modbus-simulator
      port: 502
      unit_id: 1
      poll_interval_ms: 1000
      timeout_ms: 1000
      points:
        - asset: site01.line01.press01
          tag: pressure
          unit: bar
          table: holding_registers
          address: 0        # ZERO-BASED PDU address, not 40001
          type: uint16
          scale: 0.1
        - asset: site01.line01.press01
          tag: operating
          unit: ""
          table: coils
          address: 0
          type: bool
```

Modbus register decoding includes `uint16`, `int16`, `uint32`, `int32`, and `float32`; 32-bit values use documented configurable register/word order (and byte order only if explicitly supported). Validate address boundaries (including multi-register widths), unit ID, unsupported table/type combinations, intervals, timeouts, and collisions.

## Modbus behavior

- Prefer a maintained Modbus library (evaluate libmodbus before committing to a dependency). Keep the transport library private to the adapter target.
- Per device: serialize request/response traffic, enforce finite timeouts, use bounded backoff on transport failures, and support deterministic stop and reconnect.
- Coils and discrete inputs yield booleans. Register values are decoded/scaled into the existing variant type.
- Modbus normal responses have no source timestamp: publish `source_timestamp: null` or omit it consistently with existing serialization and leave the timestamp optional in the domain.
- A successful register read gives `GOOD` quality. Protocol exceptions, timeouts and unavailable devices must appear in connection/item diagnostics. Decide explicitly whether a last known value marked BAD may be published; do not invent a numeric value and do not silently mark stale data GOOD.
- Document that Modbus TCP is plaintext and unauthenticated, suitable only for isolated demo networks until deployment controls are added.

## Incremental implementation

1. Common ingress model and tests; adapt OPC UA without changing MQTT output.
2. Backward-compatible YAML configuration; Modbus-only and mixed validation.
3. Modbus codec tests, then Modbus TCP polling/reconnection adapter.
4. Runtime multi-source lifecycle wiring and per-source diagnostics.
5. Deterministic Modbus simulator, Compose demo, integration and E2E tests for mixed acquisition and recovery.
6. README, runbook, architecture and changelog updates; CI and release verification.

## Release acceptance

- Unmodified MVP 1 YAML and Compose demo still work.
- Modbus TCP -> normalized MQTT payload includes `protocol: "modbus_tcp"`.
- OPC UA and Modbus TCP can run simultaneously in one process; mappings never collide.
- Reconnect, invalid register/exception, timeout and shutdown scenarios are covered.
- Existing and new CI tests pass. No claim of production security or persistent delivery.

## Deferred

SQLite persistence, MQTT/OPC UA TLS, ISA-95 expansion, Sparkplug B and Prometheus metrics.
