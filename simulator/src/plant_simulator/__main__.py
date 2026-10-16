"""Entry point: ``python -m plant_simulator``."""

from __future__ import annotations

import asyncio
import logging
import signal
import sys
from typing import Sequence

from plant_simulator import logging_setup, settings as settings_module
from plant_simulator.server import NamespaceIndexError, PlantServer
from plant_simulator.settings import Settings, SettingsError

log = logging.getLogger("plant_simulator")


async def _serve(settings: Settings) -> None:
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signum, stop.set)

    server = PlantServer(settings)
    await server.start()
    try:
        await server.run(stop)
    finally:
        await server.stop()


def main(argv: Sequence[str] | None = None) -> int:
    try:
        settings = settings_module.load(argv)
    except SettingsError as error:
        print(f"plant_simulator: invalid configuration: {error}", file=sys.stderr)
        return 2

    logging_setup.configure(settings.log_level, settings.log_format)
    try:
        asyncio.run(_serve(settings))
    except NamespaceIndexError as error:
        log.critical(str(error), extra={"event": "startup_failed", "error": str(error)})
        return 1
    except OSError as error:
        log.critical(
            "could not open OPC UA endpoint",
            extra={"event": "startup_failed", "endpoint": settings.endpoint, "error": str(error)},
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
