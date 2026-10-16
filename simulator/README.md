# Plant Simulator

Deterministic OPC UA server (Python, [`asyncua`](https://github.com/FreeOpcUa/opcua-asyncio) 1.1.8)
that simulates `Site01/Line01` with a hydraulic press (`Press01`) and an 850 °C heat-treatment
furnace (`Oven01`). It is the data source for the MVP 1 gateway.

## Run

Docker:

```bash
docker build -t plant-simulator simulator
docker run --rm -p 4840:4840 plant-simulator
```

Local (Python ≥ 3.12):

```bash
cd simulator
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt -e '.[dev]'
python -m plant_simulator --log-format text
python -m pytest
```

Endpoint: `opc.tcp://<host>:4840/`. Security: `None`, anonymous only (development setup, see
`docs/ARCHITECTURE.md` §7). On startup, asyncua logs two warnings about missing signing and
encryption policies. These are expected for this configuration.

## Settings

Each setting is an environment variable with a matching command-line flag (`--port`, `--seed`, …).
Flags override the environment.

| Variable             | Default   | Meaning                                                |
|----------------------|-----------|--------------------------------------------------------|
| `SIM_HOST`           | `0.0.0.0` | Bind address                                           |
| `SIM_PORT`           | `4840`    | TCP port                                               |
| `SIM_TICK_MS`        | `500`     | Tick period; also the fixed simulation step `dt`       |
| `SIM_SEED`           | `42`      | Seed for all random streams                            |
| `SIM_QUALITY_EVENTS` | `1`       | `0` disables scripted Bad/Uncertain quality windows    |
| `SIM_LOG_LEVEL`      | `INFO`    | `DEBUG` also logs every value written                  |
| `SIM_LOG_FORMAT`     | `json`    | `json` (one object per line) or `text`                 |

An invalid setting exits with code 2 and a clear message.

## Address space

Namespace URI `urn:industrial-edge-gateway:plant-simulator`. The simulator registers this namespace
through asyncua, checks that the returned index is **2**, and exits with an error if it is not.
The gateway configuration contract (`configs/gateway.example.yaml`) uses `ns=2`.

```text
Objects
└── Site01                 ns=2;s=Site01
    └── Line01             ns=2;s=Line01
        ├── Press01        ns=2;s=Line01.Press01
        └── Oven01         ns=2;s=Line01.Oven01
```

| NodeId                                | Type     | Unit  | Behaviour |
|---------------------------------------|----------|-------|-----------|
| `ns=2;s=Line01.Press01.State`           | String   |       | `STOPPED` / `STARTING` / `RUNNING` / `IDLE` / `FAULT` |
| `ns=2;s=Line01.Press01.Pressure`        | Double   | bar   | Hydraulic pressure. First-order lag (τ 2 s) toward 180 bar when RUNNING, 40 bar when STARTING/IDLE, 0 bar when STOPPED/FAULT, plus bounded correlated drift. Range 0–250 bar |
| `ns=2;s=Line01.Press01.CycleTime`       | Double   | s     | Duration of the last completed stroke (nominal 4.0 ± 0.3 s; slower at low pressure). Reads `0.0` until the first stroke completes |
| `ns=2;s=Line01.Press01.ProductionCount` | UInt32   | units | +1 per completed stroke, only while RUNNING |
| `ns=2;s=Line01.Oven01.State`            | String   |       | Same state set. STARTING lasts until the temperature is within ±15 °C of the setpoint |
| `ns=2;s=Line01.Oven01.Temperature`      | Double   | degC  | Heats at ≤ 8 °C/s in STARTING, holds 850 ± 3 °C in RUNNING/IDLE, cools toward 25 °C (τ 300 s) in STOPPED/FAULT |
| `ns=2;s=Line01.Oven01.ProductionCount`  | UInt32   | units | +1 per 30 s of RUNNING at temperature |

All variables are read-only. Numeric variables have an `EngineeringUnits` property (UNECE codes).
The counters start at 0 when the process starts, never decrease while it runs, and wrap at 2³²
like a PLC counter.

### Operating scenario

States follow a fixed script for each machine, which loops forever (`src/plant_simulator/scenario.py`):

- **Press01** (260 s loop): STARTING 5 s → RUNNING 120 s → IDLE 20 s → RUNNING 90 s → FAULT 15 s → STOPPED 10 s
- **Oven01**: STARTING (until at temperature; ~100 s from cold) → RUNNING 180 s → IDLE 30 s →
  RUNNING 120 s → FAULT 20 s → STOPPED 20 s

## Values, status and timestamps

Every tick advances the model by exactly `dt`. The model never reads the wall clock.

**Writes.** A variable is written only when its value or status code differs from what was last
written. Each write is a complete `DataValue` that explicitly sets `Value` (with an explicit
variant type), `StatusCode`, `SourceTimestamp` and `ServerTimestamp`. Initial values are written
before the endpoint opens.

**SourceTimestamp** is the UTC wall-clock time at which the simulator last produced, or updated,
that OPC UA `DataValue`. It does not change on ticks where the variable was not rewritten. On those
ticks the model still runs, but the timestamp is not a "last sampled" time. It always records the
last change to the value or status. `ServerTimestamp` is set to the same instant at write time.
All variables written in the same tick share one timestamp.

**Status codes.** Normally `Good`. With `SIM_QUALITY_EVENTS=1` (the default) the script adds:

- `Press01.Pressure`: `BadSensorFailure` from 5 s to 12 s into each FAULT phase. The value holds
  its last reported reading; only the status changes.
- `Oven01.Temperature`: `UncertainSensorNotAccurate` from 30 s to 40 s into the second RUNNING
  phase of each loop.

`State` and `ProductionCount` are always `Good`.

## Determinism

All randomness comes from `random.Random`, seeded per asset from `SIM_SEED`. The same seed, the same
`SIM_TICK_MS` and the same number of ticks produce identical values, states and status codes.
Changing the seed changes only the noise, not the state script. Timestamps are wall-clock time and
are deliberately not part of this guarantee. Tick scheduling jitter does not affect values, because
`dt` is fixed. If a tick runs over its period, the simulator logs a `tick_overrun` warning and
resynchronises instead of catching up.

## Docker healthcheck

The image's `HEALTHCHECK` opens a plain TCP connection to the port. It is a basic process/listener
liveness check only: it shows that the process is up and accepting TCP connections. It does **not**
open an OPC UA session or read a node, so it is not proof that OPC UA reads work. `tests/test_server.py`
covers those.

## Logging

One JSON object per line on stdout, with the fields `timestamp`, `level`, `component`
(`plant-simulator`), `event`, `logger`, `message`, and context fields where they apply (`endpoint`,
`namespace_uri`, `namespace_index`, `asset`, `tag`, `from`, `to`, `error`). The INFO events are
`server_started` (endpoint, namespace and all NodeIds), `state_transition`, `quality_change` and
`server_stopped`. Individual values are logged only at DEBUG (`value_written`).

## Tests

`python -m pytest` (from `simulator/`):

- `test_model.py`: 24 h of simulated time (172,800 ticks, no sleeps). Checks counter monotonicity,
  that counters increment only while RUNNING, credible ranges, smoothness, every state occurring,
  and the quality windows.
- `test_determinism.py`: identical sequences for the same seed, and independent per-asset streams.
- `test_node_contract.py`: the NodeIds and units match `configs/gateway.example.yaml` exactly.
- `test_server.py`: runs a real server on a free port with an asyncua client. Checks namespace
  index 2, browse paths, data types, status codes, Source/Server timestamps, read-only access,
  write-on-change, and failure on an unexpected namespace index.
- `test_signals.py`, `test_settings.py`: primitives and configuration.
