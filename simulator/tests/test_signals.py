import random

import pytest

from plant_simulator.signals import Ar1Noise, clamp, first_order_lag, rate_limit


def test_clamp():
    assert clamp(5.0, 0.0, 1.0) == 1.0
    assert clamp(-5.0, 0.0, 1.0) == 0.0
    assert clamp(0.5, 0.0, 1.0) == 0.5


def test_first_order_lag_reaches_63_percent_after_one_time_constant():
    value = 0.0
    for _ in range(10):
        value = first_order_lag(value, 100.0, dt=0.1, tau=1.0)
    assert value == pytest.approx(63.212, abs=1e-3)


def test_first_order_lag_never_overshoots():
    value = 0.0
    for _ in range(1000):
        value = first_order_lag(value, 100.0, dt=0.5, tau=2.0)
        assert value <= 100.0


def test_rate_limit_respects_maximum_rate():
    assert rate_limit(0.0, 100.0, max_rate=8.0, dt=0.5) == 4.0
    assert rate_limit(0.0, -100.0, max_rate=8.0, dt=0.5) == -4.0
    assert rate_limit(99.0, 100.0, max_rate=8.0, dt=0.5) == 100.0


def test_ar1_noise_is_bounded_and_correlated():
    noise = Ar1Noise(random.Random(1), dt=0.5, std=2.0, correlation_s=30.0, bound=5.0)
    previous = noise.step()
    for _ in range(20_000):
        current = noise.step()
        assert abs(current) <= 5.0
        # Strongly correlated: per-step change is a small fraction of the range.
        assert abs(current - previous) < 2.0
        previous = current
