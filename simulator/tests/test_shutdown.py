"""Bounded graceful shutdown when a client reconnects while the server is stopping.

asyncua 1.1.8 closes existing client connections, then awaits its cleanup tasks while the
listening socket still accepts, and finally waits in asyncio's Server.wait_closed(), which on
Python 3.12 waits for every connection, including one accepted in that window.
"""

import asyncio
import logging
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest
from asyncua.server.binary_server_asyncio import BinaryServer

from plant_simulator.server import GRACEFUL_SHUTDOWN_TIMEOUT_S, PlantServer
from plant_simulator.settings import Settings

SIMULATOR_SRC = Path(__file__).resolve().parents[1] / "src"


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


async def _started_server() -> tuple[PlantServer, int]:
    port = _free_port()
    server = PlantServer(Settings(host="127.0.0.1", port=port).validate())
    await server.start()
    return server, port


def _inject_reconnect_during_stop(monkeypatch, port: int, connections: list) -> None:
    """Open a client connection inside asyncua's shutdown window, deterministically.

    BinaryServer._close_tasks runs after existing transports were closed but before the
    listening socket is closed, which is exactly where a reconnecting client lands.
    """
    original = BinaryServer._close_tasks

    async def close_tasks_after_reconnect(self):
        _, writer = await asyncio.open_connection("127.0.0.1", port)
        connections.append(writer)
        local = writer.get_extra_info("sockname")
        for _ in range(200):  # wait until asyncua has accepted it
            peers = [t.get_extra_info("peername") for t in self.iserver.asyncio_transports]
            if local in peers:
                break
            await asyncio.sleep(0.01)
        else:
            raise AssertionError("injected connection was not accepted by asyncua")
        await original(self)

    monkeypatch.setattr(BinaryServer, "_close_tasks", close_tasks_after_reconnect)


async def test_stop_is_bounded_when_client_reconnects_during_shutdown(monkeypatch, caplog):
    server, port = await _started_server()
    connections: list = []
    _inject_reconnect_during_stop(monkeypatch, port, connections)
    caplog.set_level(logging.ERROR, logger="plant_simulator.server")

    started = time.monotonic()
    # The outer bound turns a regression (unbounded hang) into a failure instead of a hang.
    graceful = await asyncio.wait_for(server.stop(timeout_s=0.5), timeout=10)
    elapsed = time.monotonic() - started

    assert connections, "the reconnect was not injected"
    assert graceful is False
    assert elapsed < 0.5 + 2.0
    assert any(getattr(r, "event", None) == "shutdown_timeout" for r in caplog.records)
    for writer in connections:
        writer.close()


async def test_stop_is_graceful_without_reconnect(caplog):
    server, _ = await _started_server()
    caplog.set_level(logging.INFO, logger="plant_simulator.server")

    assert await asyncio.wait_for(server.stop(timeout_s=5), timeout=10) is True
    assert any(getattr(r, "event", None) == "server_stopped" for r in caplog.records)
    assert not any(getattr(r, "event", None) == "shutdown_timeout" for r in caplog.records)


async def test_unexpected_stop_errors_are_not_swallowed(monkeypatch):
    server, _ = await _started_server()
    real_stop = server._server.stop

    async def failing_stop():
        raise RuntimeError("unexpected asyncua failure")

    monkeypatch.setattr(server._server, "stop", failing_stop)
    with pytest.raises(RuntimeError, match="unexpected asyncua failure"):
        await server.stop()
    await real_stop()


def test_default_timeout_is_below_docker_stop_grace_period():
    assert 0 < GRACEFUL_SHUTDOWN_TIMEOUT_S < 10


@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGINT], ids=["SIGTERM", "SIGINT"])
def test_process_exits_zero_within_bound_while_client_reconnects(signum):
    port = _free_port()
    proc = subprocess.Popen(
        [sys.executable, "-m", "plant_simulator", "--host", "127.0.0.1", "--port", str(port),
         "--log-format", "text"],
        env={**os.environ, "PYTHONPATH": str(SIMULATOR_SRC)},
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    held: list[socket.socket] = []
    stop_storm = threading.Event()

    def reconnect_storm():
        # Keep connecting and holding connections, like an aggressively reconnecting client.
        while not stop_storm.is_set():
            try:
                held.append(socket.create_connection(("127.0.0.1", port), timeout=0.2))
            except OSError:
                pass
            time.sleep(0.002)

    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                assert proc.poll() is None, proc.communicate()[0]
                assert time.monotonic() < deadline, "simulator did not start"
                time.sleep(0.1)

        storm = threading.Thread(target=reconnect_storm, daemon=True)
        storm.start()
        time.sleep(0.2)
        started = time.monotonic()
        proc.send_signal(signum)
        try:
            code = proc.wait(timeout=GRACEFUL_SHUTDOWN_TIMEOUT_S + 3)
        except subprocess.TimeoutExpired:
            pytest.fail("simulator did not exit within the bounded shutdown time")
        elapsed = time.monotonic() - started
    finally:
        stop_storm.set()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        for sock in held:
            sock.close()

    output = proc.stdout.read() if proc.stdout else ""
    assert code == 0, output
    assert elapsed < GRACEFUL_SHUTDOWN_TIMEOUT_S + 3
    assert "Traceback" not in output
