"""Deterministic register model of one simulated air compressor (Site01/Line01/Compressor01).

Every value is a pure function of the tick counter (no randomness, no wall clock), so a given
tick always serves the same registers. The register map is documented in README.md and mirrored
by configs/gateway.mixed.example.yaml.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

# Operating script: RUNNING for RUN_TICKS, then STOPPED for STOP_TICKS, looping forever.
RUN_TICKS = 50
STOP_TICKS = 10
CYCLE_TICKS = RUN_TICKS + STOP_TICKS

RUNNING_TIME_START_S = 100_000  # 0x0001_86A0: exercises the high word of the uint32
SUCTION_TEMPERATURE_RAW = -125  # int16, x0.1 degC -> -12.5 degC
PRESSURE_DELTA_PA = -3500  # int32, low word first
OIL_TEMPERATURE_C = 48.25  # float32, low word first
MOTOR_CURRENT_RUNNING_A = 12.5  # float32, high word first


@dataclass(frozen=True)
class Tables:
    """One snapshot of the four Modbus tables (address -> value)."""

    coils: tuple[bool, ...]
    discrete_inputs: tuple[bool, ...]
    holding_registers: tuple[int, ...]
    input_registers: tuple[int, ...]


def to_u16(value: int) -> int:
    """Two's complement of a signed 16-bit value as a register value."""
    if not -0x8000 <= value <= 0xFFFF:
        raise ValueError(f"{value} does not fit in 16 bits")
    return value & 0xFFFF


def u32_words(value: int, high_word_first: bool) -> tuple[int, int]:
    """A 32-bit value (unsigned, or signed two's complement) as two registers."""
    if not -0x8000_0000 <= value <= 0xFFFF_FFFF:
        raise ValueError(f"{value} does not fit in 32 bits")
    raw = value & 0xFFFF_FFFF
    high, low = raw >> 16, raw & 0xFFFF
    return (high, low) if high_word_first else (low, high)


def f32_words(value: float, high_word_first: bool) -> tuple[int, int]:
    """An IEEE 754 binary32 value as two registers (big-endian bytes in each register)."""
    (raw,) = struct.unpack(">I", struct.pack(">f", value))
    return u32_words(raw, high_word_first)


class CompressorModel:
    """The simulated device. advance() moves one tick; tables() returns the current registers."""

    def __init__(self, tick_ms: int = 1000) -> None:
        if tick_ms <= 0:
            raise ValueError("tick_ms must be positive")
        self._tick_ms = tick_ms
        self._tick = 0
        self._running_ms = 0

    @property
    def tick(self) -> int:
        return self._tick

    def running(self) -> bool:
        return self._tick % CYCLE_TICKS < RUN_TICKS

    def advance(self) -> None:
        if self.running():
            self._running_ms += self._tick_ms
        self._tick += 1

    def tables(self) -> Tables:
        running = self.running()
        pressure_raw = 700 + (self._tick % 10) * 5 if running else 0  # x0.1 bar: 70.0..74.5
        running_time_s = RUNNING_TIME_START_S + self._running_ms // 1000
        motor_current = MOTOR_CURRENT_RUNNING_A if running else 0.0
        return Tables(
            coils=(
                running,  # 0 running
                False,  # 1 maintenance_mode
            ),
            discrete_inputs=(
                True,  # 0 door_closed
                False,  # 1 low_oil
            ),
            holding_registers=(
                pressure_raw,  # 0 outlet_pressure, uint16, scale 0.1
                to_u16(SUCTION_TEMPERATURE_RAW),  # 1 suction_temperature, int16, scale 0.1
                *u32_words(running_time_s, high_word_first=True),  # 2-3 running_time, uint32
                *f32_words(motor_current, high_word_first=True),  # 4-5 motor_current, float32
            ),
            input_registers=(
                *u32_words(PRESSURE_DELTA_PA, high_word_first=False),  # 0-1 pressure_delta, int32
                *f32_words(OIL_TEMPERATURE_C, high_word_first=False),  # 2-3 oil_temperature
                self._tick % 0x1_0000,  # 4 cycle_count, uint16 (wraps)
            ),
        )
