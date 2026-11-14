# Changelog

## Unreleased — MVP 3: SQLite store-and-forward (in progress)

### Added

- **Persistent message store** (`ieg::persistence::SqliteMessageStore`, target
  `gateway_persistence`): a bounded FIFO of outgoing messages (topic + payload) in a local SQLite
  file (`pending_messages` table; ids give the FIFO order and are never reused). Enqueue, peek
  (with a resume point), acknowledge (delete on confirmed delivery), per-message attempt counter,
  message and byte limits with `DropOldest`/`RejectNew` overflow policies, WAL journal with
  selectable `synchronous` level, recovery of the backlog after restart or crash, integrity check,
  schema versioning and an exclusive file lock. Not yet configurable from YAML nor used by the
  gateway runtime. Unit tests in `gateway_tests`.
- Build prerequisite: `libsqlite3-dev` (SQLite >= 3.35, checked by CMake).

## v0.2.0 — MVP 2: Modbus TCP acquisition (release candidate, not yet tagged)

Release notes and upgrade guide: [RELEASE_NOTES_v0.2.0.md](RELEASE_NOTES_v0.2.0.md).

### Added

- **Modbus TCP acquisition in the gateway**: OPC UA-only, Modbus-only and mixed deployments with
  any number of Modbus TCP devices, all publishing through the same MQTT pipeline. Each device is
  a `ModbusTcpSource` with its own worker thread; Modbus points publish `"protocol": "modbus_tcp"`
  and `"source_timestamp": null`. OPC UA topics and payloads are unchanged from MVP 1.
- **Reproducible mixed demo**: `compose.mvp2.yaml` (MVP 1 services reused via `extends`, plus a
  Modbus TCP simulator) runs the gateway with `configs/gateway.mixed.example.yaml`.
  `modbus-simulator/` is a deterministic Modbus TCP compressor (Python standard library only, own
  image, never part of the gateway image): coils, discrete inputs, holding and input registers
  with uint16, int16, uint32, int32 and float32 in both word orders, documented values and
  periodic changes, real-read health check and clean SIGTERM shutdown.
  `scripts/compose-mvp2-smoke-test.sh` (CI job `compose-demo-mvp2`) checks topics, protocols,
  values and types of both sources, a Modbus-only restart without OPC UA interruption, a Mosquitto
  restart and a clean SIGTERM shutdown; CI also runs the Modbus simulator tests. The MVP 1 demo
  (`compose.yaml`) and its smoke test are unchanged.

### Changed

- `GatewayRuntime` consumes the SDK-neutral `ieg::acquisition::IAcquisitionSource` instead of
  `IOpcUaClient`; `make_gateway_runtime()` wraps `Open62541Client` in the new `OpcUaSource`
  bridge. YAML configuration, MQTT topics and payloads, quality, timestamps, queueing and
  shutdown behavior are unchanged.
- `acquisition::Update` carries the raw protocol status as `native_status` (an integer) instead
  of a decimal string; `ItemDiagnostics` reports `source_id`, `locator` and a protocol-neutral
  `last_no_value_status`.
- `GatewayRuntime` owns several acquisition sources keyed by `(source_id, item_id)` (OPC UA keeps
  its MVP 1 item IDs); `make_gateway_runtime()` builds the OPC UA source and one Modbus source per
  device. Sources start after the publisher; a partial start failure stops every attempted
  source; shutdown stops all sources concurrently before the queue closes. Updates from unknown
  sources are never published (`RuntimeStats::unknown_source`). `validate_for_mvp1()` is renamed
  `validate_runtime_config()` and only enforces MQTT QoS 1.
- Logs: `event=started sources=<n> ...` plus `event=source_started` per source;
  `event=connection_state component=<protocol> source_id=<id> target=...` per source
  (`component=opcua`/`component=mqtt` keep their form); `event=no_value_updates` carries
  `source_id` and `diagnostic` (OPC UA keeps `node_id`). `ItemDiagnostics` adds `protocol` and
  `last_no_value_diagnostic`.
- The gateway executable links libmodbus (shared); the runtime image installs `libmodbus5`.
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
- Modbus TCP client: SDK-neutral `IModbusClient` (connect, read bits/registers into caller
  buffers as `BitBlock`/`RegisterBlock`, close) and `LibModbusClient` on libmodbus 3.1.10 (Ubuntu
  24.04 `libmodbus-dev`, exact version checked by CMake). Single owner thread, no internal threads,
  no retries; typed errors (connection failed/lost, timeout, Modbus exception code, protocol
  error) with the native `errno` or exception code; protocol request limits validated before I/O.
  Build prerequisite: `libmodbus-dev`.
- Modbus TCP acquisition source `ModbusTcpSource` (`IAcquisitionSource` for one device): one
  owner thread per device, sequential requests, read grouping of contiguous points without gaps
  (split into single-point reads after an exception about the request: 0x01, 0x02 or 0x03;
  transient exceptions keep the plan), fixed-rate polling that skips missed
  slots on overrun, bounded exponential reconnect backoff with seedable jitter, interruptible
  waits and a joined, idempotent `stop()`.
- `acquisition::Update::diagnostic`: a stable symbolic reason for Bad updates without a native
  status (e.g. `timeout`, `decode_non_finite_value`); empty for OPC UA.
- `configs/gateway.mixed.example.yaml` is the MVP 2 demo configuration: the seven MVP 1 OPC UA
  nodes plus the Modbus simulator's eleven points (host `modbus-simulator`, port 5020, unit 1).

### Fixed (release audit)

- A transient Modbus exception (e.g. 0x06 busy, 0x0B gateway target failed) on a multi-point
  block no longer splits the read plan permanently; only exceptions about the request (0x01,
  0x02, 0x03) split a block. Before, a short outage of a device behind a Modbus gateway could
  multiply the number of requests per cycle until restart.
- The Modbus reconnect backoff now keeps growing when connections succeed but no read does (a
  device that accepts TCP but never answers); before, it reconnected at the initial delay.
- The runtime stops the publisher even if its `start()` threw after starting threads.
- `ModbusTcpSource::stop()` from a callback (a contract violation) no longer races with the
  thread assignment in `start()`; a non-standard exception from the client factory is reported
  as `client_error` instead of terminating the process.

### Security

- Demo configurations are not production security configurations: anonymous MQTT without TLS,
  OPC UA security mode None, plaintext and unauthenticated Modbus TCP. Host ports bind to
  127.0.0.1; all containers run as non-root users.
- libmodbus 3.1.10 (Ubuntu 24.04) has open CVEs (no fixed release is known for CVE-2023-26793;
  CVE-2024-10918 is fixed upstream in 3.1.11). The gateway
  uses only client read functions: CVE-2024-10918 (`modbus_reply`) and CVE-2024-34244
  (`modbus_write_bits`) are not reachable; CVE-2023-26793 (`read_io_status`) is on the read path
  of coils and discrete inputs and could be triggered by a malicious or compromised Modbus
  device. Only connect to trusted devices on isolated networks (details in the release notes).

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
