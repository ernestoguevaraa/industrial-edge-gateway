"""The asyncio server over real TCP connections on 127.0.0.1."""

import asyncio
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

from modbus_simulator.healthcheck import check
from modbus_simulator.server import ModbusServer
from modbus_simulator.settings import Settings

SIMULATOR_SRC = Path(__file__).resolve().parents[1] / "src"


async def started(tick_ms: int = 100) -> ModbusServer:
    server = ModbusServer(Settings(host="127.0.0.1", port=0, tick_ms=tick_ms).validate())
    await server.start()
    return server


async def read(writer, reader, function: int, start: int, quantity: int, tid: int = 1) -> bytes:
    writer.write(struct.pack(">HHHBBHH", tid, 0, 6, 1, function, start, quantity))
    await writer.drain()
    header = await reader.readexactly(7)
    transaction, protocol, length, unit = struct.unpack(">HHHB", header)
    assert (transaction, protocol, unit) == (tid, 0, 1)
    return await reader.readexactly(length - 1)


async def test_serves_registers_and_bits_over_tcp():
    server = await started()
    reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
    try:
        holding = await read(writer, reader, 0x03, 1, 1)
        assert holding == bytes([0x03, 2, 0xFF, 0x83])
        coils = await read(writer, reader, 0x01, 0, 2, tid=2)
        assert coils[0] == 0x01
        exception = await read(writer, reader, 0x04, 10, 1, tid=3)
        assert exception == bytes([0x84, 0x02])
        assert server.requests == 3
    finally:
        writer.close()
        await server.stop()


async def test_the_model_advances_once_per_tick():
    server = await started(tick_ms=100)
    stop = asyncio.Event()
    runner = asyncio.create_task(server.run(stop))
    await asyncio.sleep(0.55)
    stop.set()
    await runner
    await server.stop()
    assert 4 <= server.model.tick <= 6


async def test_stop_closes_open_client_connections():
    server = await started()
    reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
    await read(writer, reader, 0x03, 0, 1)
    await server.stop()
    assert await asyncio.wait_for(reader.read(), timeout=2) == b""  # EOF
    writer.close()


async def test_closes_the_connection_on_a_non_modbus_frame():
    server = await started()
    reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
    writer.write(struct.pack(">HHHB", 1, 7, 6, 1) + b"\x03\x00\x00\x00\x01")
    await writer.drain()
    assert await asyncio.wait_for(reader.read(), timeout=2) == b""
    writer.close()
    await server.stop()


async def test_healthcheck_reads_a_register():
    server = await started()
    try:
        assert await asyncio.to_thread(check, "127.0.0.1", server.port, 1)
        assert not await asyncio.to_thread(check, "127.0.0.1", server.port, 9)  # other unit
    finally:
        await server.stop()


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def test_sigterm_stops_the_process_cleanly():
    port = free_port()
    env = {**os.environ, "PYTHONPATH": str(SIMULATOR_SRC), "MODBUS_SIM_HOST": "127.0.0.1",
           "MODBUS_SIM_PORT": str(port)}
    process = subprocess.Popen([sys.executable, "-m", "modbus_simulator"], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(100):
            try:
                if check("127.0.0.1", port, 1, timeout_s=0.2):
                    break
            except OSError:
                pass
            if process.poll() is not None:
                break
            time.sleep(0.05)
        process.send_signal(signal.SIGTERM)
        output, _ = process.communicate(timeout=10)
    finally:
        if process.poll() is None:
            process.kill()
    assert process.returncode == 0, output
    assert '"event": "started"' in output
    assert '"event": "stopped"' in output
