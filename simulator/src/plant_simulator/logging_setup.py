"""Structured logging: one JSON object per line, or readable text for local use."""

from __future__ import annotations

import json
import logging
import sys
from datetime import datetime, timezone

COMPONENT = "plant-simulator"

# Attributes every LogRecord has; anything else was passed through ``extra=``.
_RESERVED = set(vars(logging.LogRecord("", 0, "", 0, "", (), None))) | {"message", "asctime"}


def _timestamp(record: logging.LogRecord) -> str:
    moment = datetime.fromtimestamp(record.created, tz=timezone.utc)
    return moment.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _extras(record: logging.LogRecord) -> dict[str, object]:
    return {k: v for k, v in vars(record).items() if k not in _RESERVED and k != "event"}


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        entry: dict[str, object] = {
            "timestamp": _timestamp(record),
            "level": record.levelname,
            "component": COMPONENT,
            "event": getattr(record, "event", "log"),
            "logger": record.name,
            "message": record.getMessage(),
        }
        entry.update(_extras(record))
        if record.exc_info:
            entry["error"] = self.formatException(record.exc_info)
        return json.dumps(entry, default=str)


class TextFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        extras = " ".join(f"{k}={v}" for k, v in _extras(record).items())
        line = (
            f"{_timestamp(record)} {record.levelname:<7} {getattr(record, 'event', 'log')}: "
            f"{record.getMessage()}"
        )
        if extras:
            line = f"{line} {extras}"
        if record.exc_info:
            line = f"{line}\n{self.formatException(record.exc_info)}"
        return line


def configure(level: str, fmt: str) -> None:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(JsonFormatter() if fmt == "json" else TextFormatter())
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(level)
    # asyncua is very chatty at INFO; keep its warnings and errors only.
    logging.getLogger("asyncua").setLevel(logging.WARNING)
