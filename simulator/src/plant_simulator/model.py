"""Deterministic equipment models for Line01.

The model never reads the wall clock and draws all randomness from seeded
``random.Random`` instances, so a given (seed, dt, tick count) always yields the
same sequence of values, states and qualities.
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from types import MappingProxyType
from typing import Mapping

from plant_simulator.nodes import LINE, NODE_SPECS, OVEN, PRESS
from plant_simulator.scenario import (
    OVEN_SCRIPT,
    PRESS_SCRIPT,
    MachineState,
    Quality,
    ScenarioRunner,
)
from plant_simulator.signals import Ar1Noise, clamp, first_order_lag, rate_limit

UINT32_MOD = 2**32

_HYDRAULICS_ON = (MachineState.STARTING, MachineState.RUNNING, MachineState.IDLE)
_HEATER_ON = (MachineState.STARTING, MachineState.RUNNING, MachineState.IDLE)


@dataclass(frozen=True)
class Sample:
    value: str | float | int
    quality: Quality


@dataclass(frozen=True)
class Snapshot:
    tick: int
    sim_time_s: float
    # Keyed by NodeSpec.identifier, e.g. "Line01.Press01.Pressure".
    samples: Mapping[str, Sample]


def _asset_rng(seed: int, asset: str) -> random.Random:
    # String seeds are hashed with SHA-512 by random.Random, so this is stable
    # across processes and independent of PYTHONHASHSEED. A per-asset stream
    # means adding equipment never perturbs existing assets.
    return random.Random(f"{seed}:{asset}")


class PressModel:
    """Hydraulic press: system pressure, stroke cycle time, part counter."""

    PRESSURE_MIN_BAR = 0.0
    PRESSURE_MAX_BAR = 250.0
    PRESSURE_TARGET_BAR = {
        MachineState.STOPPED: 0.0,
        MachineState.FAULT: 0.0,
        MachineState.STARTING: 40.0,
        MachineState.IDLE: 40.0,
        MachineState.RUNNING: 180.0,
    }
    PRESSURE_TAU_S = 2.0
    NOMINAL_PRESSURE_BAR = 170.0
    NOMINAL_CYCLE_S = 4.0
    CYCLE_JITTER_S = 0.3

    def __init__(self, seed: int, dt: float, quality_events: bool) -> None:
        self._rng = _asset_rng(seed, PRESS)
        self._dt = dt
        self._quality_events = quality_events
        self.scenario = ScenarioRunner(PRESS_SCRIPT)
        self._drift = Ar1Noise(self._rng, dt, std=2.0, correlation_s=30.0, bound=5.0)
        self._ripple = Ar1Noise(self._rng, dt, std=0.4, correlation_s=1.5, bound=1.2)
        self._pressure = 0.0
        self._reported_pressure = 0.0
        self._stroke_progress_s = 0.0
        self._stroke_elapsed_s = 0.0
        self._planned_cycle_s = self._plan_cycle()
        self.cycle_time_s = 0.0  # last completed cycle; 0.0 until the first one completes
        self.production_count = 0
        self.state = self.scenario.state
        self.pressure_quality = Quality.GOOD

    def _plan_cycle(self) -> float:
        return self.NOMINAL_CYCLE_S + self._rng.uniform(-self.CYCLE_JITTER_S, self.CYCLE_JITTER_S)

    def step(self) -> None:
        dt = self._dt
        state = self.scenario.state
        self.state = state

        drift = self._drift.step()
        ripple = self._ripple.step()
        target = self.PRESSURE_TARGET_BAR[state]
        if state is MachineState.RUNNING:
            target += drift
        self._pressure = first_order_lag(self._pressure, target, dt, self.PRESSURE_TAU_S)
        hydraulic_fraction = self._pressure / self.PRESSURE_TARGET_BAR[MachineState.RUNNING]
        measured = self._pressure + ripple * hydraulic_fraction if state in _HYDRAULICS_ON else self._pressure
        measured = clamp(measured, self.PRESSURE_MIN_BAR, self.PRESSURE_MAX_BAR)

        quality = self.scenario.quality_for("Pressure") if self._quality_events else Quality.GOOD
        self.pressure_quality = quality
        if quality is not Quality.BAD_SENSOR_FAILURE:
            # A failed sensor holds its last reported value; only the status changes.
            self._reported_pressure = round(measured, 2)

        if state is MachineState.RUNNING:
            # Low pressure slows the stroke, so cycle time reacts to the hydraulics.
            speed = clamp(self._pressure / self.NOMINAL_PRESSURE_BAR, 0.05, 1.0)
            self._stroke_elapsed_s += dt
            self._stroke_progress_s += dt * speed
            if self._stroke_progress_s >= self._planned_cycle_s:
                overshoot_s = (self._stroke_progress_s - self._planned_cycle_s) / speed
                self.cycle_time_s = round(self._stroke_elapsed_s - overshoot_s, 3)
                self.production_count = (self.production_count + 1) % UINT32_MOD
                self._stroke_progress_s = overshoot_s * speed
                self._stroke_elapsed_s = overshoot_s
                self._planned_cycle_s = self._plan_cycle()
        elif state in (MachineState.FAULT, MachineState.STOPPED):
            # An interrupted stroke is aborted; the part is not counted.
            self._stroke_progress_s = 0.0
            self._stroke_elapsed_s = 0.0

        self.scenario.advance(dt)

    def samples(self) -> dict[str, Sample]:
        return {
            "State": Sample(self.state.value, Quality.GOOD),
            "Pressure": Sample(self._reported_pressure, self.pressure_quality),
            "CycleTime": Sample(self.cycle_time_s, Quality.GOOD),
            "ProductionCount": Sample(self.production_count, Quality.GOOD),
        }


class OvenModel:
    """Continuous heat-treatment furnace with an 850 degC setpoint."""

    SETPOINT_C = 850.0
    AMBIENT_C = 25.0
    TOLERANCE_C = 15.0
    TEMPERATURE_MIN_C = 0.0
    TEMPERATURE_MAX_C = 1000.0
    MAX_HEAT_RATE_C_PER_S = 8.0
    HOLD_TAU_S = 20.0
    COOL_TAU_S = 300.0
    TRAY_INTERVAL_S = 30.0

    def __init__(self, seed: int, dt: float, quality_events: bool) -> None:
        self._rng = _asset_rng(seed, OVEN)
        self._dt = dt
        self._quality_events = quality_events
        self.scenario = ScenarioRunner(OVEN_SCRIPT)
        self._wander = Ar1Noise(self._rng, dt, std=1.5, correlation_s=60.0, bound=3.0)
        self._measurement = Ar1Noise(self._rng, dt, std=0.2, correlation_s=2.0, bound=0.5)
        self._temperature = self.AMBIENT_C
        self._tray_elapsed_s = 0.0
        self.reported_temperature = self.AMBIENT_C
        self.production_count = 0
        self.state = self.scenario.state
        self.temperature_quality = Quality.GOOD

    @property
    def at_temperature(self) -> bool:
        return abs(self._temperature - self.SETPOINT_C) <= self.TOLERANCE_C

    def step(self) -> None:
        dt = self._dt
        state = self.scenario.state
        self.state = state

        wander = self._wander.step()
        noise = self._measurement.step()
        if state is MachineState.STARTING:
            self._temperature = rate_limit(
                self._temperature, self.SETPOINT_C, self.MAX_HEAT_RATE_C_PER_S, dt
            )
        elif state in _HEATER_ON:
            self._temperature = first_order_lag(
                self._temperature, self.SETPOINT_C + wander, dt, self.HOLD_TAU_S
            )
        else:
            self._temperature = first_order_lag(
                self._temperature, self.AMBIENT_C, dt, self.COOL_TAU_S
            )
        self.reported_temperature = round(
            clamp(self._temperature + noise, self.TEMPERATURE_MIN_C, self.TEMPERATURE_MAX_C), 1
        )
        self.temperature_quality = (
            self.scenario.quality_for("Temperature") if self._quality_events else Quality.GOOD
        )

        if state is MachineState.RUNNING and self.at_temperature:
            self._tray_elapsed_s += dt
            if self._tray_elapsed_s + 1e-9 >= self.TRAY_INTERVAL_S:
                self._tray_elapsed_s -= self.TRAY_INTERVAL_S
                self.production_count = (self.production_count + 1) % UINT32_MOD

        self.scenario.advance(dt, ready=self.at_temperature)

    def samples(self) -> dict[str, Sample]:
        return {
            "State": Sample(self.state.value, Quality.GOOD),
            "Temperature": Sample(self.reported_temperature, self.temperature_quality),
            "ProductionCount": Sample(self.production_count, Quality.GOOD),
        }


class PlantModel:
    """Line01: advances every machine by one fixed ``dt`` per tick."""

    def __init__(self, seed: int, dt: float, *, quality_events: bool = True) -> None:
        if dt <= 0:
            raise ValueError("dt must be positive")
        self.dt = dt
        self.tick = 0
        self.press = PressModel(seed, dt, quality_events)
        self.oven = OvenModel(seed, dt, quality_events)

    @property
    def sim_time_s(self) -> float:
        return round(self.tick * self.dt, 6)

    def step(self) -> Snapshot:
        self.press.step()
        self.oven.step()
        self.tick += 1
        return self.snapshot()

    def snapshot(self) -> Snapshot:
        """Current values. Machine states are those in effect during the last tick."""
        samples: dict[str, Sample] = {}
        for asset, machine in ((PRESS, self.press), (OVEN, self.oven)):
            for name, sample in machine.samples().items():
                samples[f"{LINE}.{asset}.{name}"] = sample
        assert samples.keys() == {spec.identifier for spec in NODE_SPECS}
        return Snapshot(self.tick, self.sim_time_s, MappingProxyType(samples))
