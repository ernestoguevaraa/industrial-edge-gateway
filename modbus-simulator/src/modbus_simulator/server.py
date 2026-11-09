"""asyncio Modbus TCP server around the deterministic compressor model."""

from __future__ import annotations

import asyncio
import contextlib
import logging

from modbus_simulator.model import CompressorModel
from modbus_simulator.protocol import MBAP_SIZE, FramingError, frame, handle_pdu, parse_header
from modbus_simulator.settings import Settings

log = logging.getLogger("modbus_simulator")

# Bound for closing client connections at shutdown.
SHUTDOWN_TIMEOUT_S = 3.0


class ModbusServer:
    """Serves the model's tables; the model advances one tick per tick period."""

    def __init__(self, settings: Settings) -> None:
        self._settings = settings
        self._model = CompressorModel(settings.tick_ms)
        self._server: asyncio.Server | None = None
        self._writers: set[asyncio.StreamWriter] = set()
        self._requests = 0

    @property
    def port(self) -> int:
        """The bound port (useful with port 0)."""
        if self._server is None or not self._server.sockets:
            raise RuntimeError("server is not started")
        return int(self._server.sockets[0].getsockname()[1])

    @property
    def requests(self) -> int:
        return self._requests

    @property
    def model(self) -> CompressorModel:
        return self._model

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._serve_connection, self._settings.host, self._settings.port
        )
        log.info(
            "listening",
            extra={
                "event": "started",
                "endpoint": f"{self._settings.host}:{self.port}",
                "unit_id": self._settings.unit_id,
                "tick_ms": self._settings.tick_ms,
            },
        )

    async def run(self, stop: asyncio.Event) -> None:
        """Advances the model on a fixed tick grid until `stop` is set."""
        loop = asyncio.get_running_loop()
        period = self._settings.tick_ms / 1000.0
        next_tick = loop.time() + period
        while not stop.is_set():
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(stop.wait(), timeout=max(0.0, next_tick - loop.time()))
            if stop.is_set():
                break
            self._model.advance()
            next_tick += period

    async def stop(self) -> None:
        """Stops accepting, closes every client connection and waits (bounded) for them."""
        if self._server is None:
            return
        self._server.close()
        for writer in list(self._writers):
            writer.close()
        with contextlib.suppress(TimeoutError):
            await asyncio.wait_for(self._server.wait_closed(), timeout=SHUTDOWN_TIMEOUT_S)
        log.info("stopped", extra={"event": "stopped", "requests": self._requests})
        self._server = None

    async def _serve_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        peer = writer.get_extra_info("peername")
        self._writers.add(writer)
        log.info("client connected", extra={"event": "client_connected", "peer": str(peer)})
        reason = "closed by client"
        try:
            while True:
                header = parse_header(await reader.readexactly(MBAP_SIZE))
                pdu = await reader.readexactly(header.pdu_size)
                response = handle_pdu(
                    header.unit_id, self._settings.unit_id, pdu, self._model.tables()
                )
                self._requests += 1
                writer.write(frame(header, response))
                await writer.drain()
        except asyncio.IncompleteReadError:
            pass
        except FramingError as error:
            reason = f"invalid frame: {error}"
        except (ConnectionError, OSError) as error:
            reason = f"connection error: {error}"
        finally:
            self._writers.discard(writer)
            writer.close()
            with contextlib.suppress(ConnectionError, OSError):
                await writer.wait_closed()
            log.info(
                "client disconnected",
                extra={"event": "client_disconnected", "peer": str(peer), "reason": reason},
            )
