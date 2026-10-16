"""Small, pure signal-shaping primitives used by the equipment models."""

from __future__ import annotations

import math
import random


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def first_order_lag(current: float, target: float, dt: float, tau: float) -> float:
    """Exact discrete step of ``tau * dx/dt = target - x`` over ``dt`` seconds."""
    return current + (target - current) * (1.0 - math.exp(-dt / tau))


def rate_limit(current: float, target: float, max_rate: float, dt: float) -> float:
    """Move toward ``target`` by at most ``max_rate * dt``."""
    max_step = max_rate * dt
    return current + clamp(target - current, -max_step, max_step)


class Ar1Noise:
    """Bounded, time-correlated noise (discrete Ornstein-Uhlenbeck process).

    ``std`` is the stationary standard deviation, ``correlation_s`` the time
    constant over which successive values stay correlated, and ``bound`` a hard
    limit on the magnitude. Successive samples differ by a small amount, unlike
    independent random draws.
    """

    def __init__(
        self, rng: random.Random, dt: float, std: float, correlation_s: float, bound: float
    ) -> None:
        self._rng = rng
        self._phi = math.exp(-dt / correlation_s)
        self._sigma = std * math.sqrt(1.0 - self._phi * self._phi)
        self._bound = bound
        self.value = 0.0

    def step(self) -> float:
        self.value = clamp(
            self._phi * self.value + self._rng.gauss(0.0, self._sigma), -self._bound, self._bound
        )
        return self.value
