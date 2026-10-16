"""Model behaviour over 24 hours of simulated time (no real sleeps)."""

from dataclasses import dataclass

import pytest

from plant_simulator.model import OvenModel, PlantModel, PressModel
from plant_simulator.scenario import MachineState, Quality

DT = 0.5
TICKS_24H = int(24 * 3600 / DT)
STEADY_TICKS = int(10 / DT)  # RUNNING for at least 10 s

PRESS_STATE = "Line01.Press01.State"
PRESSURE = "Line01.Press01.Pressure"
CYCLE_TIME = "Line01.Press01.CycleTime"
PRESS_COUNT = "Line01.Press01.ProductionCount"
OVEN_STATE = "Line01.Oven01.State"
TEMPERATURE = "Line01.Oven01.Temperature"
OVEN_COUNT = "Line01.Oven01.ProductionCount"


@dataclass(frozen=True)
class Tick:
    press_state: str
    pressure: float
    pressure_quality: Quality
    cycle_time: float
    press_count: int
    oven_state: str
    temperature: float
    temperature_quality: Quality
    oven_count: int
    oven_at_temperature: bool


def _run(ticks: int, *, quality_events: bool = True) -> list[Tick]:
    model = PlantModel(seed=42, dt=DT, quality_events=quality_events)
    history = []
    for _ in range(ticks):
        samples = model.step().samples
        history.append(
            Tick(
                press_state=samples[PRESS_STATE].value,
                pressure=samples[PRESSURE].value,
                pressure_quality=samples[PRESSURE].quality,
                cycle_time=samples[CYCLE_TIME].value,
                press_count=samples[PRESS_COUNT].value,
                oven_state=samples[OVEN_STATE].value,
                temperature=samples[TEMPERATURE].value,
                temperature_quality=samples[TEMPERATURE].quality,
                oven_count=samples[OVEN_COUNT].value,
                oven_at_temperature=model.oven.at_temperature,
            )
        )
    return history


@pytest.fixture(scope="module")
def day() -> list[Tick]:
    return _run(TICKS_24H)


def _pairs(history):
    return zip(history, history[1:])


def test_counters_never_decrease(day):
    for prev, cur in _pairs(day):
        assert cur.press_count >= prev.press_count
        assert cur.oven_count >= prev.oven_count


def test_press_counts_only_while_running(day):
    for prev, cur in _pairs(day):
        if cur.press_count != prev.press_count:
            assert cur.press_state == MachineState.RUNNING.value
            assert cur.press_count == prev.press_count + 1


def test_oven_counts_only_while_running_at_temperature(day):
    for prev, cur in _pairs(day):
        if cur.oven_count != prev.oven_count:
            assert cur.oven_state == MachineState.RUNNING.value
            assert cur.oven_at_temperature
            assert cur.oven_count == prev.oven_count + 1


def test_counters_hold_when_not_running(day):
    for prev, cur in _pairs(day):
        if cur.press_state != MachineState.RUNNING.value:
            assert cur.press_count == prev.press_count
        if cur.oven_state != MachineState.RUNNING.value:
            assert cur.oven_count == prev.oven_count


def test_production_rates_are_plausible(day):
    press_running_s = sum(DT for t in day if t.press_state == "RUNNING")
    oven_running_s = sum(DT for t in day if t.oven_state == "RUNNING")
    # ~4 s per stroke; ~30 s per tray.
    assert day[-1].press_count == pytest.approx(press_running_s / 4.0, rel=0.05)
    assert day[-1].oven_count == pytest.approx(oven_running_s / 30.0, rel=0.05)


def test_values_stay_within_credible_ranges(day):
    for t in day:
        assert PressModel.PRESSURE_MIN_BAR <= t.pressure <= PressModel.PRESSURE_MAX_BAR
        assert OvenModel.TEMPERATURE_MIN_C <= t.temperature <= OvenModel.TEMPERATURE_MAX_C
        # Nominal 4.0 +/- 0.3 s; the first stroke after a pressure ramp is slower.
        assert t.cycle_time == 0.0 or 3.5 <= t.cycle_time <= 6.0


def _steady_pairs(history, state_attr, min_ticks=STEADY_TICKS):
    """(prev, cur) pairs where the machine has been RUNNING for at least ``min_ticks``."""
    run_length = 0
    pairs = []
    for prev, cur in _pairs(history):
        run_length = run_length + 1 if getattr(cur, state_attr) == "RUNNING" else 0
        if run_length > min_ticks:
            pairs.append((prev, cur))
    return pairs


def test_steady_running_values_stay_near_operating_point(day):
    press = _steady_pairs(day, "press_state")
    oven = [(p, c) for p, c in _steady_pairs(day, "oven_state") if c.oven_at_temperature]
    assert press and oven
    for _, cur in press:
        assert 170.0 <= cur.pressure <= 190.0
        assert 3.5 <= cur.cycle_time <= 5.0
    for _, cur in oven:
        assert 835.0 <= cur.temperature <= 865.0


def test_signals_change_smoothly(day):
    # Even during state transients the per-tick change is bounded by the
    # process dynamics (lag / heating rate), unlike independent random draws.
    for prev, cur in _pairs(day):
        if prev.pressure_quality is Quality.GOOD and cur.pressure_quality is Quality.GOOD:
            assert abs(cur.pressure - prev.pressure) <= 45.0
        assert abs(cur.temperature - prev.temperature) <= 5.0


def test_steady_state_changes_are_small(day):
    press = _steady_pairs(day, "press_state")
    oven = [(p, c) for p, c in _steady_pairs(day, "oven_state") if c.oven_at_temperature]
    assert max(abs(c.pressure - p.pressure) for p, c in press) < 3.0
    assert max(abs(c.temperature - p.temperature) for p, c in oven) < 1.5


def test_running_values_are_not_constant(day):
    running_pressures = {t.pressure for t in day if t.press_state == "RUNNING"}
    running_temps = {t.temperature for t in day if t.oven_state == "RUNNING"}
    assert len(running_pressures) > 100
    assert len(running_temps) > 50


def test_cycle_time_tracks_completed_cycles(day):
    for prev, cur in _pairs(day):
        if cur.cycle_time != prev.cycle_time:
            assert cur.press_count == prev.press_count + 1


def test_every_state_occurs(day):
    expected = {s.value for s in MachineState}
    assert {t.press_state for t in day} == expected
    assert {t.oven_state for t in day} == expected


def test_oven_heats_at_bounded_rate_from_cold_start():
    history = _run(400)
    assert history[0].oven_state == "STARTING"
    first_running = next(i for i, t in enumerate(history) if t.oven_state == "RUNNING")
    # 25 -> ~835 degC at 8 degC/s takes ~100 s (200 ticks).
    assert 190 <= first_running <= 215
    assert history[first_running].oven_at_temperature


def test_quality_events_occur_and_bad_value_is_held(day):
    bad = [i for i, t in enumerate(day) if t.pressure_quality is Quality.BAD_SENSOR_FAILURE]
    uncertain = [t for t in day if t.temperature_quality is Quality.UNCERTAIN_SENSOR_NOT_ACCURATE]
    assert bad and uncertain
    for i in bad:
        assert day[i].press_state == "FAULT"
        if day[i - 1].pressure_quality is Quality.BAD_SENSOR_FAILURE:
            assert day[i].pressure == day[i - 1].pressure
    assert all(t.oven_state == "RUNNING" for t in uncertain)


def test_quality_events_can_be_disabled():
    history = _run(4000, quality_events=False)
    assert all(t.pressure_quality is Quality.GOOD for t in history)
    assert all(t.temperature_quality is Quality.GOOD for t in history)


def test_initial_snapshot_values():
    snapshot = PlantModel(seed=42, dt=DT).snapshot()
    assert snapshot.tick == 0
    assert snapshot.samples[PRESS_COUNT].value == 0
    assert snapshot.samples[CYCLE_TIME].value == 0.0
    assert snapshot.samples[TEMPERATURE].value == OvenModel.AMBIENT_C
    assert all(s.quality is Quality.GOOD for s in snapshot.samples.values())


def test_invalid_dt_rejected():
    with pytest.raises(ValueError):
        PlantModel(seed=1, dt=0.0)
