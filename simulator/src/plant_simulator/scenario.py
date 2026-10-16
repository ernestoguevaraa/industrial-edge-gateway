"""Scripted, looping operating scenarios for each machine.

State transitions are scripted rather than random so behaviour is reproducible
and a demo shows every state within a few minutes.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass


class MachineState(str, enum.Enum):
    STOPPED = "STOPPED"
    STARTING = "STARTING"
    RUNNING = "RUNNING"
    IDLE = "IDLE"
    FAULT = "FAULT"


class Quality(enum.Enum):
    """Protocol-neutral quality; the server maps it to OPC UA status codes."""

    GOOD = "Good"
    UNCERTAIN_SENSOR_NOT_ACCURATE = "UncertainSensorNotAccurate"
    BAD_SENSOR_FAILURE = "BadSensorFailure"


@dataclass(frozen=True)
class QualityEvent:
    """Overrides the quality of one variable during a window inside a phase."""

    variable: str
    quality: Quality
    start_s: float
    end_s: float


@dataclass(frozen=True)
class Phase:
    state: MachineState
    # Minimum time in the phase. A STARTING phase additionally waits until the
    # machine reports it is ready (e.g. the oven reached its setpoint band).
    duration_s: float
    quality_events: tuple[QualityEvent, ...] = ()


PRESS_SCRIPT: tuple[Phase, ...] = (
    Phase(MachineState.STARTING, 5.0),
    Phase(MachineState.RUNNING, 120.0),
    Phase(MachineState.IDLE, 20.0),
    Phase(MachineState.RUNNING, 90.0),
    Phase(
        MachineState.FAULT,
        15.0,
        (QualityEvent("Pressure", Quality.BAD_SENSOR_FAILURE, 5.0, 12.0),),
    ),
    Phase(MachineState.STOPPED, 10.0),
)

OVEN_SCRIPT: tuple[Phase, ...] = (
    Phase(MachineState.STARTING, 5.0),
    Phase(MachineState.RUNNING, 180.0),
    Phase(MachineState.IDLE, 30.0),
    Phase(
        MachineState.RUNNING,
        120.0,
        (QualityEvent("Temperature", Quality.UNCERTAIN_SENSOR_NOT_ACCURATE, 30.0, 40.0),),
    ),
    Phase(MachineState.FAULT, 20.0),
    Phase(MachineState.STOPPED, 20.0),
)


class ScenarioRunner:
    """Walks a looping phase script in simulated time."""

    def __init__(self, script: tuple[Phase, ...]) -> None:
        if not script:
            raise ValueError("scenario script must not be empty")
        self._script = script
        self._index = 0
        self._time_in_phase = 0.0

    @property
    def phase(self) -> Phase:
        return self._script[self._index]

    @property
    def state(self) -> MachineState:
        return self.phase.state

    def quality_for(self, variable: str) -> Quality:
        for event in self.phase.quality_events:
            if event.variable == variable and event.start_s <= self._time_in_phase < event.end_s:
                return event.quality
        return Quality.GOOD

    def advance(self, dt: float, *, ready: bool = True) -> None:
        """Advance by ``dt``; leave the phase once its duration (and readiness) is met."""
        self._time_in_phase += dt
        phase = self.phase
        if self._time_in_phase + 1e-9 < phase.duration_s:
            return
        if phase.state is MachineState.STARTING and not ready:
            return
        self._index = (self._index + 1) % len(self._script)
        self._time_in_phase = 0.0
