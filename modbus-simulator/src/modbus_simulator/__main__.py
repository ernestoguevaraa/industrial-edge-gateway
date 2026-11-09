"""Entry point: ``python -m modbus_simulator``."""

from __future__ import annotations

import asyncio
import logging
import signal
import sys
from typing import Sequence

from modbus_simulator import logging_setup, settings as settings_module
from modbus_simulator.server import ModbusServer
from modbus_simulator.settings import Settings, SettingsError

log = logging.getLogger("modbus_simulator")


async def _serve(settings: Settings) -> None:
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signum, stop.set)
    server = ModbusServer(settings)
    await server.start()
    try:
        await server.run(stop)
    finally:
        await server.stop()


def main(argv: Sequence[str] | None = None) -> int:
    try:
        settings = settings_module.load(argv)
    except SettingsError as error:
        print(f"modbus_simulator: invalid configuration: {error}", file=sys.stderr)
        return 2
    logging_setup.configure(settings.log_level, settings.log_format)
    try:
        asyncio.run(_serve(settings))
    except OSError as error:
        log.critical(
            "could not open the Modbus TCP endpoint",
            extra={"event": "startup_failed", "port": settings.port, "error": str(error)},
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
