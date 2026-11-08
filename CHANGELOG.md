# Changelog

## Unreleased — MVP 2 (in progress)

### Changed

- `GatewayRuntime` consumes the SDK-neutral `ieg::acquisition::IAcquisitionSource` instead of
  `IOpcUaClient`; `make_gateway_runtime()` wraps `Open62541Client` in the new `OpcUaSource`
  bridge. YAML configuration, MQTT topics and payloads, quality, timestamps, queueing and
  shutdown behavior are unchanged.
- `acquisition::Update` carries the raw protocol status as `native_status` (an integer) instead
  of a decimal string; `ItemDiagnostics` reports `source_id`, `locator` and a protocol-neutral
  `last_no_value_status`.
- The `event=started` log line also reports `source_id`.
- Configuration: `opcua` and `assets` are optional when Modbus TCP devices are configured under
  the new optional `modbus.devices` section (source ID, host, port, unit ID, poll interval,
  timeout and points with table, zero-based offset, type, word order and scale). The loader
  validates unique source IDs, global `(asset, tag)` uniqueness, address ranges and widths,
  table/type combinations, finite non-zero scales, intervals and timeouts, and rejects
  configurations without any source. `GatewayConfig::opcua` is now `std::optional`.
- Transport-independent Modbus data decoder (`ieg::modbus::decode`, target
  `gateway_modbus_decoder`): bool from coils/discrete inputs, uint16/int16, uint32/int32 and
  float32 with explicit word order, optional scale, and an explicit `DecodeResult`/`DecodeError`
  policy (insufficient data, table/type mismatch, invalid word order or scale, non-finite values).
  Not yet used by the runtime.
- `configs/gateway.mixed.example.yaml` documents a mixed OPC UA + Modbus TCP deployment. Modbus
  TCP acquisition is **not implemented yet**: the gateway rejects configured Modbus devices at
  startup with exit code 2. The MVP 1 example and Compose demo are unchanged.

## v0.1.0 — MVP 1: OPC UA → MQTT vertical slice

First release of the Industrial Edge Gateway: a C++20 gateway that subscribes to OPC UA data,
normalizes it into a protocol-neutral telemetry model and publishes it as JSON over MQTT 5. It is
a reference implementation, not a production-hardened product.

### Highlights

- **OPC UA client** on open62541 1.4.21: subscribes to the configured nodes, preserves source
  timestamps and raw StatusCodes, maps quality to GOOD/UNCERTAIN/BAD, and reconnects
  automatically with bounded backoff, restoring all monitored items.
- **MQTT 5 publisher** on Eclipse Paho MQTT C 1.3.16: QoS 1, retain false; a point is reported
  delivered only after a successful PUBACK; MQTT 5 reason codes are mapped to retry
  dispositions; automatic reconnect.
- **Gateway runtime**: a bounded in-memory queue decouples OPC UA callbacks from the single
  publishing thread and provides best-effort, in-memory buffering of the most recent points
  during short MQTT outages (oldest dropped first). Bounded SIGINT/SIGTERM shutdown; exit codes
  0 (requested shutdown), 1 (start failure), 2 (configuration error).
- **Validated YAML configuration** with clear startup errors; MVP 1 requires MQTT QoS 1.
- **Deterministic Python OPC UA plant simulator** (asyncua) with a press and a heat-treatment
  oven, scripted machine states and scripted Bad/Uncertain quality windows.
- **One-command demo**: `docker compose up --build -d --wait` runs the simulator, the gateway
  and Eclipse Mosquitto.

### Quality

GitHub Actions builds with GCC and Clang (Debug and Release, warnings as errors) and runs unit
tests, integration tests against the real Python simulator and a real Mosquitto broker,
end-to-end tests of the gateway executable, clang-tidy, clang-format, the simulator tests and a
Compose smoke test. The smoke test also restarts Mosquitto and the simulator and verifies that
the running gateway reconnects and resumes publishing.

### Known limitations (by design for MVP 1)

- No authentication and no TLS; the demo broker accepts anonymous clients.
- No persistence or store-and-forward: queued points are lost on restart and failed publishes
  are not retried.
- A single OPC UA endpoint with a static node configuration.
- MQTT QoS 1 only.
- Built and tested on Linux (Ubuntu 24.04, x86_64).
- Not a production-hardened deployment.

### Planned

Modbus (MVP 2), SQLite store-and-forward (MVP 3), metrics and observability (MVP 4), ISA-95
asset modeling, TLS/mTLS and Sparkplug B. None of these are implemented in v0.1.0.
