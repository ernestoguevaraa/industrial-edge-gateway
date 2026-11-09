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
    # Unprivileged default so the container can run as a non-root user (502 needs privileges).
    port: int = 5020
    unit_id: int = 1
    tick_ms: int = 1000
    log_level: str = "INFO"
    log_format: str = "json"

    def validate(self) -> "Settings":
        if not self.host:
            raise SettingsError("host must not be empty")
        if not 0 <= self.port <= 65535:
            raise SettingsError(f"port must be in 0..65535, got {self.port}")
        if not 1 <= self.unit_id <= 247:
            raise SettingsError(f"unit id must be in 1..247, got {self.unit_id}")
        if not 100 <= self.tick_ms <= 60_000:
            raise SettingsError(f"tick must be in 100..60000 ms, got {self.tick_ms}")
        if self.log_level not in LOG_LEVELS:
            raise SettingsError(f"log level must be one of {', '.join(LOG_LEVELS)}")
        if self.log_format not in LOG_FORMATS:
            raise SettingsError(f"log format must be one of {', '.join(LOG_FORMATS)}")
        return self


def _int(environ: Mapping[str, str], name: str, default: int) -> int:
    text = environ.get(name)
    if text is None:
        return default
    try:
        return int(text)
    except ValueError:
        raise SettingsError(f"{name} must be an integer, got {text!r}") from None


def load(argv: Sequence[str] | None = None, environ: Mapping[str, str] | None = None) -> Settings:
    env = os.environ if environ is None else environ
    defaults = Settings(
        host=env.get("MODBUS_SIM_HOST", Settings.host),
        port=_int(env, "MODBUS_SIM_PORT", Settings.port),
        unit_id=_int(env, "MODBUS_SIM_UNIT_ID", Settings.unit_id),
        tick_ms=_int(env, "MODBUS_SIM_TICK_MS", Settings.tick_ms),
        log_level=env.get("MODBUS_SIM_LOG_LEVEL", Settings.log_level).upper(),
        log_format=env.get("MODBUS_SIM_LOG_FORMAT", Settings.log_format),
    )
    parser = argparse.ArgumentParser(prog="modbus-simulator", exit_on_error=False)
    parser.add_argument("--host", default=defaults.host)
    parser.add_argument("--port", type=int, default=defaults.port)
    parser.add_argument("--unit-id", type=int, default=defaults.unit_id)
    parser.add_argument("--tick-ms", type=int, default=defaults.tick_ms)
    parser.add_argument("--log-level", default=defaults.log_level, type=str.upper)
    parser.add_argument("--log-format", default=defaults.log_format)
    try:
        args = parser.parse_args(argv)
    except (argparse.ArgumentError, SystemExit) as error:
        raise SettingsError(str(error)) from None
    return Settings(
        host=args.host,
        port=args.port,
        unit_id=args.unit_id,
        tick_ms=args.tick_ms,
        log_level=args.log_level,
        log_format=args.log_format,
    ).validate()
