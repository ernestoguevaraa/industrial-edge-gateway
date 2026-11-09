# Industrial Edge Gateway v0.2.0 — MVP 2: Modbus TCP acquisition

> **Status: release candidate.** These notes describe the content of PR #2
> (`feature/mvp2-modbus-tcp`). The `v0.2.0` tag has not been created yet.

v0.2.0 adds **Modbus TCP** as a second acquisition protocol next to OPC UA. Any number of Modbus
TCP devices and the OPC UA server feed one bounded queue and one MQTT 5 publisher. OPC UA-only,
Modbus-only and mixed deployments are supported. The v0.1.0 configuration, MQTT topics and
payloads are unchanged.

Like v0.1.0, this is a reference implementation and **not a production-hardened product**: no
authentication, no TLS and no persistence (see [Security](#security)).

## What's new

- **Common acquisition model.** OPC UA and every Modbus device are `IAcquisitionSource`s of the
  same runtime. Points are keyed by `(source_id, item_id)`, and updates from unknown sources are
  never published.
- **Modbus TCP client and poller** (libmodbus 3.1.10):
  - Reads coils, discrete inputs, holding registers and input registers.
  - Contiguous points of the same table are grouped into one request, with no gaps and within
    the protocol limits.
  - Polls on a fixed rate and skips missed slots when a cycle overruns.
  - Reconnects with bounded exponential backoff and jitter.
  - Each device has one worker thread; requests are strictly sequential.
- **Data decoding.** Supports `bool`, `uint16`, `int16`, `uint32`, `int32` and `float32`:
  - big-endian bytes inside each register;
  - an explicit `high_word_first` or `low_word_first` word order for 32-bit values;
  - an optional `scale`.

  Integers stay integers when no `scale` is set.
- **Diagnostics without invented values.**
  - A Modbus failure produces a `BAD` update without a value; it is counted, not published.
  - Real Modbus exception codes are kept as the native status.
  - Timeouts, disconnections and decode errors get stable symbolic diagnostics such as
    `timeout` or `decode_non_finite_value`.
  - Modbus points have `"source_timestamp": null`, because Modbus has no sample timestamps.
- **Configuration.** A new optional `modbus.devices` section. `opcua` and `assets` become
  optional when Modbus devices are configured. The loader validates source IDs, global
  `(asset, tag)` uniqueness, zero-based offsets and widths, table/type combinations, word order,
  scale, intervals and timeouts, and reports every problem at once.
- **Mixed demo.** `compose.mvp2.yaml` adds a deterministic Modbus TCP compressor simulator
  (`modbus-simulator/`) to the MVP 1 services and publishes 7 OPC UA and 11 Modbus topics.
- **Tests and CI.** CI now has 10 jobs:
  - GCC and Clang, Debug and Release, with warnings as errors;
  - clang-tidy and clang-format;
  - both simulators' tests;
  - both Compose smoke tests.

  The smoke tests cover Modbus-only restarts without OPC UA interruption, and Mosquitto restarts.

## Requirements

- **Build:** Linux (built and tested on Ubuntu 24.04 x86_64), CMake ≥ 3.25, GCC 13 or Clang 18
  with C++20.
  - New prerequisite: **`libmodbus-dev`** (libmodbus **3.1.10**, the Ubuntu 24.04 package). CMake
    checks the exact version.
  - Existing prerequisite: `libmbedtls-dev`.
- **Run:** the gateway links libmodbus dynamically, so the target system needs `libmodbus5`. The
  Docker image already contains it.
- **Demos:** Docker with the Compose plugin. The demo ports bind to 127.0.0.1: 1883 (MQTT), 4840
  (OPC UA) and 5020 (Modbus TCP, MVP 2 demo).

## Demos

MVP 1 (OPC UA only, unchanged):

```bash
docker compose up --build -d --wait
docker compose exec mosquitto mosquitto_sub -t 'industrial/#' -v
docker compose down
```

MVP 2 (OPC UA + Modbus TCP). It uses the same host ports, so stop the MVP 1 demo first:

```bash
docker compose -f compose.mvp2.yaml up --build -d --wait
docker compose -f compose.mvp2.yaml exec mosquitto mosquitto_sub -t 'industrial/#' -v
docker compose -f compose.mvp2.yaml restart modbus-simulator   # Modbus recovers, OPC UA continues
docker compose -f compose.mvp2.yaml down
```

If a port is already in use on the host (for example a local Mosquitto service on 1883), set
`IEG_MQTT_HOST_PORT`, `IEG_OPCUA_HOST_PORT` or `IEG_MODBUS_HOST_PORT` before `up`. Smoke tests:
`scripts/compose-smoke-test.sh` and `scripts/compose-mvp2-smoke-test.sh`. The register map of the
Modbus simulator is in [modbus-simulator/README.md](modbus-simulator/README.md). Operations are
described in [docs/RUNBOOK.md](docs/RUNBOOK.md), section 17.

## Upgrading from v0.1.0

**No action needed:**
- The v0.1.0 YAML configuration is valid unchanged and behaves identically.
- MQTT topics, payload fields and semantics are unchanged (QoS 1, retain false). OPC UA payloads
  still publish `"protocol": "opcua"`.
- The `compose.yaml` demo is unchanged.

**Build and deployment:**
- Install `libmodbus-dev` to build, and `libmodbus5` where the gateway runs.
- The MVP 2 demo builds its gateway image as `ieg/edge-gateway:mvp2`. The MVP 1 demo keeps
  `ieg/edge-gateway:mvp1`.

**Logs** (if you parse them):
- `event=started` now reads `sources=<n> mqtt_broker=... items=...`. The OPC UA endpoint moved to
  the new `event=source_started source_id=opcua-main protocol=opcua target=<endpoint> items=<n>`
  line.
- `event=connection_state` lines for acquisition sources gain `source_id=`. The lines
  `component=opcua` and `component=mqtt` keep their form.
- `event=no_value_updates` gains `source_id=` and `diagnostic=`. OPC UA lines keep `node_id=`.

**Embedding the C++ libraries** (not needed for the executable):
- `GatewayRuntime` takes a vector of `IAcquisitionSource`s. A single-source constructor remains.
- `validate_for_mvp1()` is now `validate_runtime_config()`, and only enforces MQTT QoS 1.
- `GatewayConfig::opcua` is `std::optional`.
- `ItemDiagnostics` uses `locator` instead of `node_id`, and adds `source_id`, `protocol` and
  `last_no_value_diagnostic`.
- `RuntimeStats` adds `unknown_source`.
- `acquisition::Update` carries an integer `native_status` and a `diagnostic` string.

## Known limitations

- **Modbus scope:** Modbus TCP only (no RTU) and reads only (no writes). One unit ID per
  configured device; several devices may point to the same host.
- **Modbus offsets are zero-based PDU addresses.** Holding register "40001" is
  `table: holding_registers`, `offset: 0`. Writing `offset: 40001` is accepted but reads PDU
  address 40001.
- **Exception codes ≥ 12:** libmodbus 3.1.10 does not preserve them, so they are reported as
  `protocol_error`.
- **Shutdown timing:**
  - A blocked Modbus request cannot be cancelled. Stopping waits up to one request timeout
    (`timeout_ms`), in parallel across devices.
  - A connect attempt can take `timeout_ms` per resolved address.
  - DNS resolution of a host name has no guaranteed upper bound. Use IP addresses where shutdown
    latency matters.
- **Buffering:** the in-memory queue is bounded, and points are lost on restart. There is no
  store-and-forward, and failed publishes are not retried.
- **Platform:** built and tested on Linux only (Ubuntu 24.04, x86_64).

## Security

- **The demos are development environments, not production security configurations:**
  - MQTT accepts anonymous clients without TLS.
  - OPC UA uses SecurityPolicy None, anonymous only.
  - Modbus TCP is plaintext and unauthenticated by design.
  - Mitigations in the demos: all published host ports bind to 127.0.0.1, the services talk over
    the private Compose network, and every container runs as a non-root user.
- **libmodbus 3.1.10** (Ubuntu 24.04 package) has CVEs that Ubuntu lists as unpatched or under
  evaluation for 24.04. The gateway imports only libmodbus client functions (connect/close, the
  four read functions and settings; verified on the binary).
  - **CVE-2024-10918** (`modbus_reply`, server side; fixed upstream in 3.1.11) and
    **CVE-2024-34244** (`modbus_write_bits`): **not reachable** from the gateway.
  - **CVE-2023-26793** (heap overflow reported in `read_io_status`, CVSS 9.8; no fixed release
    known): **on the read path** of coils and discrete inputs. The attacker would be a malicious
    or compromised Modbus device the gateway polls.
    - The gateway limits request quantities and passes exactly-sized buffers.
    - A fuzzing run of the read functions against an AddressSanitizer build of libmodbus 3.1.10
      with malformed responses (about 12,000 connections) found no memory error. This is
      evidence, not proof.
    - **Recommendation:** connect only to trusted devices on isolated networks, and track a fixed
      libmodbus release.

## Fixed during the release audit

- A transient Modbus exception (e.g. 0x06 busy or 0x0B gateway target failed) no longer splits a
  multi-point read block permanently. Only exceptions about the request split a block: 0x01
  illegal function, 0x02 illegal data address and 0x03 illegal data value.
- The Modbus reconnect backoff keeps growing for a device that accepts TCP but never answers.
- Shutdown robustness:
  - The runtime stops the publisher even after a failed `start()`.
  - `ModbusTcpSource::stop()` from a callback no longer races with the thread assignment in
    `start()`.
  - A non-standard exception from the client factory is reported instead of terminating the
    process.

See [CHANGELOG.md](CHANGELOG.md) for the complete list of changes.
