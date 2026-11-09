# Modbus TCP Simulator

Deterministic Modbus TCP server (Python standard library, asyncio) that simulates one air
compressor, `Site01/Line01/Compressor01`. It is the Modbus data source of the MVP 2 mixed demo
(`compose.mvp2.yaml`) and is never part of the gateway image.

> Demo only. Modbus TCP is plaintext and unauthenticated; the simulator accepts any client. Do
> not expose it outside an isolated development network.

## Run

Docker:

```bash
docker build -t modbus-simulator modbus-simulator
docker run --rm -p 5020:5020 modbus-simulator
```

Local (Python ≥ 3.12):

```bash
cd modbus-simulator
python -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'
python -m modbus_simulator --log-format text
python -m pytest
```

SIGINT/SIGTERM stop it cleanly (connections closed, exit code 0).

## Settings

Each setting is an environment variable with a matching command-line flag; flags override the
environment. An invalid setting exits with code 2.

| Variable                | Flag           | Default   | Meaning                                  |
|-------------------------|----------------|-----------|------------------------------------------|
| `MODBUS_SIM_HOST`       | `--host`       | `0.0.0.0` | Bind address                             |
| `MODBUS_SIM_PORT`       | `--port`       | `5020`    | TCP port (unprivileged; not 502)         |
| `MODBUS_SIM_UNIT_ID`    | `--unit-id`    | `1`       | Unit ID served (1..247)                  |
| `MODBUS_SIM_TICK_MS`    | `--tick-ms`    | `1000`    | Model step                               |
| `MODBUS_SIM_LOG_LEVEL`  | `--log-level`  | `INFO`    | `DEBUG`, `INFO`, `WARNING`, `ERROR`      |
| `MODBUS_SIM_LOG_FORMAT` | `--log-format` | `json`    | `json` (one object per line) or `text`   |

## Protocol

Function codes 01 (Read Coils), 02 (Read Discrete Inputs), 03 (Read Holding Registers) and
04 (Read Input Registers), each with the protocol limits (2000 bits, 125 registers). Exception
responses: 0x01 for other function codes, 0x02 for addresses outside the map below, 0x03 for an
invalid quantity, and 0x0B for requests addressed to another unit ID. Registers use big-endian
bytes; 32-bit values use the word order given in the table.

## Register map (zero-based addresses)

The values are a pure function of the tick counter `n` (ticks since start, 1 s by default). The
compressor runs for 50 ticks, then stops for 10 ticks, and the cycle repeats.

| Table | Address | Tag | Type | Raw value | Published value (gateway) |
|---|---|---|---|---|---|
| coils | 0 | `running` | bool | `n mod 60 < 50` | `true`/`false` |
| coils | 1 | `maintenance_mode` | bool | `0` | `false` |
| discrete inputs | 0 | `door_closed` | bool | `1` | `true` |
| discrete inputs | 1 | `low_oil` | bool | `0` | `false` |
| holding | 0 | `outlet_pressure` | uint16, scale 0.1 | running: `700 + 5 * (n mod 10)`; stopped: `0` | 70.0..74.5 bar in 0.5 steps; 0.0 stopped |
| holding | 1 | `suction_temperature` | int16, scale 0.1 | `-125` (`0xFF83`) | `-12.5` degC |
| holding | 2-3 | `running_time` | uint32, high word first | `100000 + running seconds` | integer s, ≥ 100000, +1 per running second |
| holding | 4-5 | `motor_current` | float32, high word first | running: `12.5` (`0x4148 0x0000`); stopped: `0.0` | `12.5` / `0.0` A |
| input | 0-1 | `pressure_delta` | int32, low word first | `-3500` (`0xF254 0xFFFF`) | `-3500` Pa |
| input | 2-3 | `oil_temperature` | float32, low word first | `48.25` (`0x0000 0x4241`) | `48.25` degC |
| input | 4 | `cycle_count` | uint16 | `n mod 65536` | integer, +1 per tick |

`configs/gateway.mixed.example.yaml` maps exactly these points (unit ID 1, port 5020). Every
read of an unmapped address answers with exception 0x02.
