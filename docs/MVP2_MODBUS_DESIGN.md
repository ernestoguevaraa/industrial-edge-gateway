# MVP 2 — Modbus TCP acquisition design

Status: Implemented through increment 2.8 — common ingress model (2.2), multi-source YAML (2.3), data decoder (2.4), Modbus TCP client (2.5), polling source (2.6), multi-source runtime (2.7) and the reproducible mixed Compose demo (2.8); release work remains\
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

Increment 2.7 extended the runtime to several sources (see "Multi-source runtime" below).

## Configuration (increment 2.3, implemented)

The loader accepts OPC UA-only, Modbus-only and mixed deployments; the v0.1.0 YAML is unchanged
and still valid. See `configs/gateway.mixed.example.yaml` (separate from the MVP 1 example).

**Runtime status:** since increment 2.7 every configured source runs (OPC UA-only, Modbus-only
and mixed). Until then the runtime rejected Modbus devices at startup.

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

## Modbus TCP transport (increment 2.5, implemented)

`ieg::modbus::IModbusClient` (`gateway/include/ieg/modbus/modbus_client.hpp`, target
`gateway_modbus_api`) is the SDK-neutral client interface; `LibModbusClient`
(`libmodbus_client.hpp`, target `gateway_modbus_tcp`) implements it with libmodbus. Neither is
used by the runtime yet (no poller), and the gateway executable does not link libmodbus.

### Dependency

libmodbus **3.1.10**, from the Ubuntu 24.04 package `libmodbus-dev` (3.1.10-1ubuntu1, universe).
CMake requires exactly this upstream version (parsed from `modbus-version.h`) and links the
shared library through the imported target `libmodbus::libmodbus`, privately to
`gateway_modbus_tcp`. Rationale: libmodbus has only an autotools build, the project already takes
Mbed TLS from the same distribution, and dynamic linking keeps LGPL-2.1 compliance simple. CI,
the gateway image build stage and the runbook install `libmodbus-dev`.

Known issues of the Ubuntu 24.04 package (its tracker lists noble as unpatched or "needs
evaluation"; CVE-2024-10918 is fixed upstream in 3.1.11, while no fixed release is known for
CVE-2023-26793):
- CVE-2024-10918 (`modbus_reply`) and CVE-2024-34244 (`modbus_write_bits`): **not reachable**.
  The gateway imports only libmodbus client functions (connect/close, `modbus_read_bits`,
  `modbus_read_input_bits`, `modbus_read_registers`, `modbus_read_input_registers` and settings;
  verified with `nm -D`), and the tests' Modbus server is a separate POSIX implementation.
- CVE-2023-26793 (heap overflow reported in `read_io_status`): **on the read path** of coils and
  discrete inputs. A malicious or compromised Modbus device is the attacker model. The gateway
  limits quantities to the protocol maximum and passes exactly-sized buffers; fuzzing the four
  read functions against an AddressSanitizer build of libmodbus 3.1.10 with randomly malformed
  responses (about 12,000 connections) found no memory error. This is evidence, not proof:
  connect only to trusted devices on isolated networks, and re-evaluate when a fixed libmodbus
  is available.

### Interface and contract

- `connect()` (re)establishes the connection, closing an existing one first; `connected()`;
  `close()` is idempotent and safe before `connect()`; the destructor closes.
- `read_registers(table, start, std::span<std::uint16_t> out)` (holding: FC03, input: FC04) and
  `read_bits(table, start, std::span<std::uint8_t> out)` (coils: FC01, discrete inputs: FC02)
  read `out.size()` values into the caller's buffer and return a `RegisterBlock`/`BitBlock` that
  views it and carries `table` and `start`, ready for `decode()`.
- Results are `ModbusResult<T>` (value or `ModbusError`); nothing throws for I/O failures.
- **Single owner thread.** The client is not thread-safe and creates no threads; every call,
  including `close()` and the destructor, comes from one owner thread.
- **No hidden retries or reconnects.** libmodbus' error recovery is disabled; a read never
  connects implicitly. After `ConnectionLost`, `Timeout` or `ProtocolError` the client closes the
  connection itself (a late or partial response would desynchronise the stream) and the owner
  must call `connect()` again. A Modbus exception is a valid answer and keeps the connection.
- Options come from the device configuration (`client_options_from()`): host, port, unit ID
  (sent in every MBAP header) and `timeout_ms`.

### Request limits

Validated before any I/O (`validate_read()`, also used by the test fake): the call must match the
table kind, the quantity must be 1..2000 for bits (FC01/FC02) and 1..125 for registers
(FC03/FC04), and `start + quantity` must not exceed 65536. Violations return `InvalidRequest`
without touching the connection.

### Errors

| `ModbusErrorKind` | When | Native code |
|---|---|---|
| `InvalidRequest` | rejected before I/O (limits, table kind) | – |
| `NotConnected` | read without an established connection | – |
| `ConnectionFailed` | `connect()` failed: refused, unreachable, name resolution, connect timeout | `system_errno` (e.g. `ECONNREFUSED`; `EINPROGRESS`/`ETIMEDOUT` for a connect timeout) |
| `ConnectionLost` | the connection failed during a request (reset, closed by the peer) | `system_errno` (e.g. `ECONNRESET`, `EPIPE`) |
| `Timeout` | no complete response within `timeout_ms` | `system_errno = ETIMEDOUT` |
| `ModbusException` | exception response from the server | `exception_code` (1..11), never `system_errno` |
| `ProtocolError` | response does not answer the request (transaction ID, length, value count, exception code ≥ 12) | – (libmodbus `EMB*` text in `detail`) |

libmodbus encodes exception codes as `MODBUS_ENOBASE + code` in `errno`; the client decodes them
into `exception_code` and never reports them as `system_errno`. libmodbus 3.1.10 does not
preserve exception codes ≥ 12 (they become `EMBBADEXC`), so those are `ProtocolError`.

### Timeouts and shutdown

- `connect()` waits at most `timeout_ms` for the TCP handshake **per resolved address** (a host
  name resolving to several addresses is tried address by address). Name resolution
  (`getaddrinfo`) happens before and is **not** bounded by `timeout_ms`; use IP addresses or a
  local resolver where this matters.
- A read sends the request and waits for the complete response within `timeout_ms` in total:
  the libmodbus byte timeout is disabled, so the deadline is not restarted per received chunk
  (libmodbus reuses one `timeval` across `select()` calls, which Linux decrements).
- **There is no cancellation of a blocked call.** `close()` or destruction from another thread
  while a call is running is a data race and is not supported. To shut down, the owner thread
  (the future poller) checks its stop flag between requests and then calls `close()`; stop
  latency is bounded by the call in progress (≈ `timeout_ms`, plus name resolution for
  `connect()`). Keep `timeout_ms` short for responsive shutdown. A cross-thread interrupt (e.g.
  `shutdown(2)` on the socket) is deliberately not offered, because libmodbus does not support it
  safely.
- Writes use `MSG_NOSIGNAL` (libmodbus), so a closed peer cannot raise `SIGPIPE`.

### Tests

- Unit tests: request validation, result API, error-kind names, libmodbus error classification
  (`errno`/`EMB*` → `ModbusError`), and `FakeModbusClient`
  (`gateway/tests/modbus/fake_modbus_client.hpp`), a scripted `IModbusClient` with the same
  validation and connection rules for future poller tests.
- Deterministic loopback tests (`libmodbus_client_test.cpp`) run the real `LibModbusClient`
  against an in-process Modbus TCP server on 127.0.0.1 with an ephemeral port
  (`modbus_test_server.hpp`, plain POSIX sockets, no external process): reads of every table
  including maximum quantities at the end of the address space, unit ID, unavailable server,
  timeouts and explicit reconnection, exception codes, protocol errors, connection loss,
  reconnect while connected, idempotent close and destruction.

## Modbus TCP acquisition source (increment 2.6, implemented)

`ModbusTcpSource` (`gateway/include/ieg/modbus/modbus_tcp_source.hpp`, target
`gateway_modbus_source`) implements `acquisition::IAcquisitionSource` for one device by combining
an `IModbusClient` (by default `LibModbusClient`) with the decoder. Since increment 2.7 the
runtime creates one per configured device.

### Items, updates and diagnostics

- An item's locator is `modbus_point_locator(point)` = `"<asset>/<tag>"`, unique within a device;
  `start()` accepts any non-empty subset of the device's points (unique IDs and locators).
- Updates carry the source ID and item ID. `source_timestamp` is always empty: Modbus responses
  have no sample timestamp (the runtime still adds its edge timestamp).
- Success: quality `Good` and the decoded `TelemetryValue` (integers stay integers without
  scale).
- Failure: quality `Bad`, no value. `native_status` is set **only** for real Modbus exception
  responses (the exception code); nothing is manufactured for local failures.
- `acquisition::Update` gained a protocol-neutral `diagnostic` string (stable, symbolic, never
  numeric; empty when there is nothing to report). The Modbus source uses
  `to_string(ModbusErrorKind)` for read failures (`timeout`, `connection_lost`,
  `protocol_error`, `modbus_exception`, ...) and `decode_<error>` for decoding failures (e.g.
  `decode_non_finite_value`). The OPC UA source leaves it empty (its raw StatusCode is in
  `native_status`).
- Connection state is reported as for OPC UA: `Connecting` per attempt, `Connected`, and
  `Disconnected` with a detail such as `timeout: ...`, `connection_failed: ...` or
  `client_error: ...`; a deliberate `stop()` reports nothing.

### Polling design

- One worker thread per device owns the client: it is created through the factory, used, closed
  and destroyed only on that thread. Requests are strictly sequential; callbacks run on the
  worker, serialized.
- `start()` validates the items, builds the read plan and starts the worker without waiting for
  TCP. The worker connects and then polls.
- Schedule (steady clock): the first cycle starts right after connecting; cycle *n* starts at
  `t0 + n * poll_interval`. **Overrun policy:** if a cycle ends after one or more slots, the
  missed slots are skipped (no back-to-back catch-up) and the next cycle starts at the first
  future slot of the same grid. All waits are interruptible (`std::stop_token` +
  `condition_variable_any`); there are no busy loops (the poll interval is at least 100 ms in the
  configuration and must be positive).
- Reconnection: after a failed connect or a lost connection, the worker waits
  `min(initial * 2^n, max)` (defaults 500 ms .. 30 s), multiplied by a jitter factor in
  `[1 - jitter, 1]` (default 0.2, seedable for deterministic tests), then reconnects explicitly.
  The failure counter resets only after a connection that delivered at least one successful
  read, so a device that accepts TCP but never answers keeps backing off exponentially instead
  of being reconnected at a fixed rate.

### Read grouping policy

- Points of the same table are merged into one request only while they are **contiguous or
  overlapping**; a request never covers an address that no requested point uses (no gaps), never
  mixes tables and never exceeds 125 registers / 2000 bits (configurable lower). Points are
  ordered by address; a 32-bit point is never split across requests.
- If a multi-point block answers with a Modbus exception **about the request** — 0x01 Illegal
  Function, 0x02 Illegal Data Address (e.g. one address in the range is not implemented) or 0x03
  Illegal Data Value — the block is split into single-point reads immediately (same cycle) and
  permanently, so one bad address only affects its own point. Other exception codes (0x04 device
  failure, 0x06 busy, 0x0A/0x0B gateway path/target, ...) describe a transient device state: the
  block's items are reported `Bad` with that code and the plan is kept, so an outage of a device
  behind a Modbus gateway never multiplies the number of requests.
- A timeout, connection loss or protocol error closes the connection (client contract): the items
  of the failed block and of the rest of the cycle get `Bad` with that diagnostic, `Disconnected`
  is reported, and the worker backs off before reconnecting. No request is retried within a
  cycle.

### Shutdown limits

`stop()` is idempotent, safe before `start()` and after a failed start; when it returns the
worker has been joined, so no callback is running or will run. It must not be called from a
callback (detected: it then only requests the stop). Latency:

- waiting for the next cycle or a backoff: immediate (measured 0 ms);
- a request in progress: completes or times out first — at most `timeout_ms` (measured
  297–301 ms with `timeout_ms = 300`); the worker checks the stop request before every request,
  so at most one request is in flight even though a cycle may contain several;
- a connect attempt: `timeout_ms` per resolved address, plus DNS resolution (`getaddrinfo`),
  which has no guaranteed upper bound.

### Tests

- `poll_plan_test.cpp`: grouping (contiguity, gaps, tables, overlaps, limits, splitting, end of
  the address space) and the schedule function.
- `modbus_tcp_source_test.cpp`: the source with a thread-safe fake device and a manually stepped
  clock (deterministic, no real sleeps for timing): typed updates, grouping, fixed grid and
  overrun skipping, exception splitting, decode failures, timeouts and connection loss with
  reconnection, exponential backoff and its reset, deterministic jitter, client factory failures,
  client ownership by one worker thread, stop latency and no callbacks after stop.
- `modbus_tcp_source_server_test.cpp`: the real client against the deterministic in-process
  server: conversions for all four tables, exception codes on a kept connection, connection loss
  and timeout recovery, unavailable server, and measured stop latency.
- The Modbus tests also pass under AddressSanitizer/UBSan and ThreadSanitizer.

## Multi-source runtime (increment 2.7, implemented)

`GatewayRuntime` owns a collection of `IAcquisitionSource`s that share one bounded queue, one
consumer thread and one MQTT publisher. `make_gateway_runtime()` builds an `OpcUaSource` if
`opcua` is configured and one `ModbusTcpSource` per `modbus.devices` entry; OPC UA-only,
Modbus-only and mixed deployments with several Modbus devices are supported. The only remaining
runtime restriction is MQTT QoS 1.

### Item identity

- Every point is keyed by `(source_id, item_id)`. Item IDs are unique per source only: the OPC UA
  source keeps the MVP 1 IDs (configuration order of assets and nodes), each Modbus device numbers
  its points `0..n-1` in configuration order and is asked for them with
  `modbus_point_locator(point)` = `"<asset>/<tag>"`.
- One immutable table holds every binding (asset, tag, unit, locator, protocol); a lookup hashes
  the source ID and indexes the item, so concurrent callbacks need no lock and no mapping is
  duplicated.
- `TelemetryPoint::protocol` is `opcua` or `modbus_tcp`. Topics are `<prefix>/<asset levels>/<tag>`
  for both, so OPC UA topics and payloads are exactly those of MVP 1; `(asset, tag)` is unique
  across sources (configuration validation), so topics never collide.
- An update from an unknown `source_id` is never published (`unknown_source`), nor one for an item
  its source was not asked for (`unknown_item`). Statuses from unknown sources are ignored and
  counted the same way.

### Lifecycle

- Construction requires exactly one source object per configured source ID (any order); missing,
  unexpected, duplicate or null sources are rejected. Nothing connects before `start()`.
- `start()`: monitor thread, publisher and consumer first, then the sources one after another.
  If a source's `start()` throws, every source whose start was attempted is stopped (also the one
  that threw), then the queue is closed and drained and the publisher stopped.
- `stop()`: all started sources are stopped **concurrently** (one short-lived thread per source),
  so shutdown waits for the slowest source rather than the sum; each source closes its client on
  its own worker thread, never on the main thread. Then the queue is closed, the consumer drains
  within `shutdown_drain_timeout_ms`, the publisher stops. Idempotent.

### Concurrency

Callbacks of different sources run concurrently. The data path uses the immutable table, atomic
counters, a per-item lock for the last diagnostic text (written only by that item's source) and
the queue's own short critical section. Status callbacks take the runtime state lock only to copy
the state and detail strings; logging happens on the monitor thread.

### Logs and diagnostics

- `event=started sources=<n> mqtt_broker=... items=...`, then one
  `event=source_started source_id=... protocol=... target=... items=...` per source.
- `event=connection_state component=<protocol> source_id=<id> target=<endpoint|host:port>
  state=... detail="..." transitions=n` per source (`component=opcua` / `component=mqtt` keep the
  MVP 1 form, with `source_id` added for sources).
- `event=no_value_updates source_id=... asset=... tag=... node_id|locator=... count=...
  last_status=0x...|none diagnostic="..."` per item with Bad updates without value; OPC UA keeps
  the key `node_id`. `item_diagnostics()` returns the same per `(source_id, locator)`, including
  `protocol`, the raw native status (OPC UA StatusCode or Modbus exception code, never mixed: the
  protocol tells them apart) and the symbolic diagnostic.
- Modbus Bad updates carry no value and no source timestamp; they are counted, not published.

### Tests

- `multi_source_runtime_test.cpp`: exact source set, start order and per-source items, partial
  start failure, equal item IDs across sources, unknown sources/items, Modbus-only, per-item
  diagnostics, concurrent callbacks from three sources, shutdown order and concurrent stopping.
- End-to-end (`gateway_end_to_end_test.cpp`): the gateway executable with the OPC UA simulator, an
  in-process Modbus TCP server and Mosquitto — mixed (MVP 1 topics unchanged, Modbus payloads,
  recovery after a Modbus outage in the same process) and Modbus-only.

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
3. Modbus codec tests, then Modbus TCP polling/reconnection adapter. *(Codec: 2.4; transport client: 2.5; polling source: 2.6.)*
4. Runtime multi-source lifecycle wiring and per-source diagnostics. *(Done: increment 2.7.)*
5. Deterministic Modbus simulator, Compose demo, integration and E2E tests for mixed acquisition and recovery. *(Done: integration and E2E tests in 2.5–2.7; `modbus-simulator/`, `compose.mvp2.yaml` and the MVP 2 smoke test in 2.8.)*
6. README, runbook, architecture and changelog updates; CI and release verification.

## Release acceptance

- Unmodified MVP 1 YAML and Compose demo still work.
- Modbus TCP -> normalized MQTT payload includes `protocol: "modbus_tcp"`.
- OPC UA and Modbus TCP can run simultaneously in one process; mappings never collide.
- Reconnect, invalid register/exception, timeout and shutdown scenarios are covered.
- Existing and new CI tests pass. No claim of production security or persistent delivery.

## Deferred

SQLite persistence, MQTT/OPC UA TLS, ISA-95 expansion, Sparkplug B and Prometheus metrics.
