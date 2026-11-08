# MVP 2 — Modbus TCP acquisition design

Status: In progress — increments 2.2 (common ingress model, OPC UA bridge, runtime migration), 2.3 (multi-source YAML configuration) and 2.4 (data decoder) implemented; Modbus transport/polling not started\
Tracking: [Issue #1](https://github.com/ernestoguevaraa/industrial-edge-gateway/issues/1)

## Motivation

MVP 1 had a protocol-independent `TelemetryPoint` and MQTT pipeline, but `GatewayRuntime` depended directly on `IOpcUaClient`, `opcua::DataChange`, OPC UA item identifiers and OPC UA mapping. Increment 2.2 removed that dependency (see "Implemented contract" below). Adding Modbus directly to that runtime would couple ingestion and orchestration to individual SDKs. Refactor the ingress contract before implementing Modbus.

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

## Implemented contract (increment 2.2)

`gateway/include/ieg/acquisition/acquisition_source.hpp` implements the proposal above with these
decisions:
- `Update::native_status` (`std::optional<std::uint32_t>`) carries the raw protocol status
  (OPC UA StatusCode; a Modbus exception code later) instead of a string, so no parsing is needed
  and the runtime keeps reporting it as `last_status=0x...`. It is opaque to the runtime.
- `IAcquisitionSource::id()` exposes the source ID; the runtime accepts updates only for the
  `(source_id, item_id)` pairs it requested.
- `stop()` is idempotent, safe before `start()` and after a failed `start()`; the runtime always
  stops the source before the publisher, including when the source's `start()` threw.
- `OpcUaSource` (source ID `opcua-main`) wraps `Open62541Client`; `make_gateway_runtime()` builds it.
  The MVP 1 YAML, MQTT topics and payloads are unchanged.

The runtime still owns exactly one source; the multi-source lifecycle is increment 4.

## Configuration (increment 2.3, implemented)

The loader accepts OPC UA-only, Modbus-only and mixed deployments; the v0.1.0 YAML is unchanged
and still valid. See `configs/gateway.mixed.example.yaml` (separate from the MVP 1 example).

**Runtime status:** Modbus configuration is parsed and fully validated, but Modbus TCP
acquisition is not implemented. `validate_for_mvp1()` and the `GatewayRuntime` constructor
reject any configured Modbus device (and Modbus-only configurations), so the gateway exits with
code 2 and `Modbus TCP acquisition is not implemented yet` instead of silently running only the
OPC UA part.

### Sources

- OPC UA is configured by `opcua` **and** `assets` together (one without the other is an error).
  Its source ID is fixed: `opcua-main` (`config::kOpcUaSourceId`).
- `modbus.devices` is an optional, non-empty list; each device is one acquisition source.
- At least one source must exist; otherwise: `no acquisition source configured`.
- `source_id`: 1–64 characters `[a-z0-9][a-z0-9_-]*`, unique across devices; `opcua-main` is
  reserved.
- `(asset, tag)` is unique across **all** sources (OPC UA nodes and every Modbus point), since it
  defines the MQTT topic.

### Device keys

| Key | Required | Range / default |
|---|---|---|
| `source_id` | yes | see above |
| `host` | yes | non-empty, no whitespace |
| `port` | no | 1–65535, default 502 |
| `unit_id` | yes | 1–255 (0 is broadcast and never answers a read) |
| `poll_interval_ms` | no | 100–3600000, default 1000 |
| `timeout_ms` | no | 10–60000, must not exceed `poll_interval_ms`; default min(1000, poll interval) |
| `points` | yes | non-empty list |

### Point keys

| Key | Required | Values |
|---|---|---|
| `asset`, `tag` | yes | same rules as OPC UA nodes |
| `unit` | no | string, default `""` |
| `table` | yes | `coils`, `discrete_inputs`, `holding_registers`, `input_registers` |
| `offset` | yes | 0–65535, **zero-based PDU address** |
| `type` | yes | `bool`, `uint16`, `int16`, `uint32`, `int32`, `float32` |
| `word_order` | 32-bit types only (required there) | `high_word_first`, `low_word_first` |
| `scale` | no, never for `bool` | finite, non-zero number |

Unknown keys are rejected (e.g. `address` instead of `offset`), and every problem is reported at
once with file, line and path.

### Data convention

- **Addressing.** `offset` is the zero-based address in the PDU, within its table. The
  traditional "register numbers" are *not* accepted as such: holding register 40001 is
  `table: holding_registers, offset: 0`, input register 30010 is `table: input_registers,
  offset: 9`. Writing `offset: 40001` is legal but addresses PDU address 40001. A 32-bit value
  occupies `offset` and `offset + 1`; it must end at or before address 65535.
- **Tables and types.** Coils and discrete inputs are `bool` only. Registers carry `uint16`,
  `int16`, `uint32`, `int32` or `float32`; reading single bits inside a register is not
  supported.
- **Byte order.** Inside each 16-bit register, bytes are big-endian (high byte first), as the
  Modbus Application Protocol requires. This is fixed and not configurable.
- **Word order.** Required for 32-bit types, rejected for others, so nothing defaults silently:
  - `high_word_first`: register `offset` holds bits 31..16 ("ABCD", Modbus-conventional).
  - `low_word_first`: register `offset` holds bits 15..0 ("CDAB", common in many PLCs).
  - Byte-swapped layouts ("BADC", "DCBA") are not supported.
- **Interpretation.** `int16`/`int32` are two's complement; `float32` is IEEE 754 binary32
  assembled from the 32-bit pattern above.
- **Published value type.** Without `scale`: `uint16`/`uint32` → unsigned integer,
  `int16`/`int32` → signed integer, `float32` → double, `bool` → boolean. With `scale`:
  `raw * scale` as a double (also for `scale: 1`).

Example: registers `[0x4049, 0x0FDB]` as `float32` with `high_word_first` decode to `0x40490FDB`
≈ 3.14159; the same registers with `low_word_first` decode to `0x0FDB4049`.

## Data decoder (increment 2.4, implemented)

`ieg::modbus::decode()` (`gateway/include/ieg/modbus/decoder.hpp`, target
`gateway_modbus_decoder`) turns the data of one Modbus read into a `TelemetryValue` following the
convention above. It is independent of sockets, transport and external libraries (only the
header-only config and domain types), and is not wired into the runtime yet.

Input is what the transport returns after parsing the PDU:
- `RegisterBlock { table, start, values }`: 16-bit register values, `values[0]` at address
  `start`. `register_from_bytes(high, low)` builds a value from its big-endian PDU byte pair.
- `BitBlock { table, start, values }`: one element per coil/discrete input, `0` = false and any
  other value = true.

Decoding:
- `uint16`/`int16` read one register; `uint32`/`int32`/`float32` read `offset` and `offset + 1`,
  combined with shifts by word order (`high_word_first`: `(r0 << 16) | r1`; `low_word_first`:
  `(r1 << 16) | r0`). The CPU's byte order never matters.
- `int16`/`int32` use `std::bit_cast` to the signed type (two's complement); `float32` uses
  `std::bit_cast<float>` (IEEE 754 binary32, enforced with `static_assert(is_iec559)`) and is
  widened exactly to `double`.
- Without `scale`: `bool` → `bool`, unsigned types → `std::uint64_t`, signed types →
  `std::int64_t`, `float32` → `double`. With `scale`: `double(raw) * scale` (always a double).
  Integer-to-double conversion is exact (at most 32 significant bits).

Error policy: `decode()` never throws for bad data or options; it returns a `DecodeResult` that
holds either the value or a `DecodeError`, and never clamps, substitutes or guesses a value:

| `DecodeError` | Cause |
|---|---|
| `InsufficientData` | the block does not contain every address of the point (offset before the block start, or offset + width past its end) |
| `TableMismatch` | the block was read from a different table than the point's |
| `TypeTableMismatch` | `bool` from registers, or a register type from coils/discrete inputs |
| `InvalidWordOrder` | `word_order` missing for a 32-bit type or present for another type |
| `InvalidScale` | `scale` zero or not finite, or present for `bool` |
| `NonFiniteValue` | `float32` NaN/±infinity, or a scaled result that overflows to infinity |

The configuration loader already rejects invalid options; the decoder checks them again so it is
safe on its own. Bounds are checked before any element is read. `to_string(DecodeError)` gives a
stable snake_case name for logs and diagnostics. Unit tests cover minimum/maximum, negative and
zero values, IEEE 754 patterns (signed zero, subnormal, extremes, NaN, infinities), both word
orders, scaling and every error; they also pass under AddressSanitizer and UBSan.

## Modbus behavior

- Prefer a maintained Modbus library (evaluate libmodbus before committing to a dependency). Keep the transport library private to the adapter target.
- Per device: serialize request/response traffic, enforce finite timeouts, use bounded backoff on transport failures, and support deterministic stop and reconnect.
- Coils and discrete inputs yield booleans. Register values are decoded/scaled into the existing variant type.
- Modbus normal responses have no source timestamp: publish `source_timestamp: null` or omit it consistently with existing serialization and leave the timestamp optional in the domain.
- A successful register read gives `GOOD` quality. Protocol exceptions, timeouts and unavailable devices must appear in connection/item diagnostics. Decide explicitly whether a last known value marked BAD may be published; do not invent a numeric value and do not silently mark stale data GOOD.
- Document that Modbus TCP is plaintext and unauthenticated, suitable only for isolated demo networks until deployment controls are added.

## Incremental implementation

1. Common ingress model and tests; adapt OPC UA without changing MQTT output. *(Done: increment 2.2.)*
2. Backward-compatible YAML configuration; Modbus-only and mixed validation. *(Done: increment 2.3; the runtime still rejects Modbus devices.)*
3. Modbus codec tests, then Modbus TCP polling/reconnection adapter. *(Codec done: increment 2.4; transport and polling pending.)*
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
