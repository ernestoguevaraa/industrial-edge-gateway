"""Runtime settings from environment variables, overridable on the command line."""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass
from typing import Mapping, Sequence

LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR")
LOG_FORMATS = ("json", "text")


class SettingsError(ValueError):
    pass


@dataclass(frozen=True)
class Settings:
    host: str = "0.0.0.0"
    port: int = 4840
    tick_ms: int = 500
    seed: int = 42
    quality_events: bool = True
    log_level: str = "INFO"
    log_format: str = "json"

    @property
    def endpoint(self) -> str:
        return f"opc.tcp://{self.host}:{self.port}/"

    @property
    def dt_s(self) -> float:
        return self.tick_ms / 1000.0

    def validate(self) -> "Settings":
        if not self.host:
            raise SettingsError("host must not be empty")
        if not 1 <= self.port <= 65535:
            raise SettingsError(f"port must be in 1..65535, got {self.port}")
        if not 10 <= self.tick_ms <= 60_000:
            raise SettingsError(f"tick_ms must be in 10..60000, got {self.tick_ms}")
        if self.log_level not in LOG_LEVELS:
            raise SettingsError(f"log level must be one of {LOG_LEVELS}, got {self.log_level!r}")
        if self.log_format not in LOG_FORMATS:
            raise SettingsError(f"log format must be one of {LOG_FORMATS}, got {self.log_format!r}")
        return self


def _int(env: Mapping[str, str], name: str, default: int) -> int:
    raw = env.get(name)
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError:
        raise SettingsError(f"{name} must be an integer, got {raw!r}") from None


def _bool(env: Mapping[str, str], name: str, default: bool) -> bool:
    raw = env.get(name)
    if raw is None or raw == "":
        return default
    normalized = raw.strip().lower()
    if normalized in ("1", "true", "yes", "on"):
        return True
    if normalized in ("0", "false", "no", "off"):
        return False
    raise SettingsError(f"{name} must be a boolean (1/0, true/false), got {raw!r}")


def from_env(env: Mapping[str, str]) -> Settings:
    defaults = Settings()
    return Settings(
        host=env.get("SIM_HOST") or defaults.host,
        port=_int(env, "SIM_PORT", defaults.port),
        tick_ms=_int(env, "SIM_TICK_MS", defaults.tick_ms),
        seed=_int(env, "SIM_SEED", defaults.seed),
        quality_events=_bool(env, "SIM_QUALITY_EVENTS", defaults.quality_events),
        log_level=(env.get("SIM_LOG_LEVEL") or defaults.log_level).upper(),
        log_format=(env.get("SIM_LOG_FORMAT") or defaults.log_format).lower(),
    )


def load(argv: Sequence[str] | None = None, env: Mapping[str, str] | None = None) -> Settings:
    """Build settings: defaults < environment < command-line flags."""
    base = from_env(os.environ if env is None else env)
    parser = argparse.ArgumentParser(
        prog="plant_simulator", description="Deterministic OPC UA plant simulator (Line01)."
    )
    parser.add_argument("--host", default=base.host, help="bind address (SIM_HOST)")
    parser.add_argument("--port", type=int, default=base.port, help="TCP port (SIM_PORT)")
    parser.add_argument("--tick-ms", type=int, default=base.tick_ms, help="tick period (SIM_TICK_MS)")
    parser.add_argument("--seed", type=int, default=base.seed, help="random seed (SIM_SEED)")
    parser.add_argument(
        "--quality-events",
        action=argparse.BooleanOptionalAction,
        default=base.quality_events,
        help="scripted Bad/Uncertain quality windows (SIM_QUALITY_EVENTS)",
    )
    parser.add_argument("--log-level", default=base.log_level, help="SIM_LOG_LEVEL")
    parser.add_argument("--log-format", default=base.log_format, help="json or text (SIM_LOG_FORMAT)")
    args = parser.parse_args(argv)
    return Settings(
        host=args.host,
        port=args.port,
        tick_ms=args.tick_ms,
        seed=args.seed,
        quality_events=args.quality_events,
        log_level=args.log_level.upper(),
        log_format=args.log_format.lower(),
    ).validate()
