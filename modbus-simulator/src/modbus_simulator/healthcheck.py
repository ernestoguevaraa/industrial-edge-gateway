"""Container health check: performs a real Modbus TCP read (FC03, holding register 0).

Exit code 0 when the simulator answers with a valid response, 1 otherwise.
"""

from __future__ import annotations

import os
import socket
import struct
import sys

from modbus_simulator.protocol import MBAP_SIZE, READ_HOLDING_REGISTERS


def _receive_exactly(connection: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed before a complete response")
        data += chunk
    return data


def check(host: str, port: int, unit_id: int, timeout_s: float = 2.0) -> bool:
    """True if the server answers the read with a normal (non-exception) response."""
    request = struct.pack(">HHHBBHH", 0x4845, 0, 6, unit_id, READ_HOLDING_REGISTERS, 0, 1)
    with socket.create_connection((host, port), timeout=timeout_s) as connection:
        connection.sendall(request)
        header = _receive_exactly(connection, MBAP_SIZE)
        transaction_id, protocol_id, length, unit = struct.unpack(">HHHB", header)
        if length < 2:
            return False
        pdu = _receive_exactly(connection, length - 1)
    return (transaction_id, protocol_id, unit) == (0x4845, 0, unit_id) and pdu[:2] == bytes(
        (READ_HOLDING_REGISTERS, 2)
    )


def main() -> int:
    port = int(os.environ.get("MODBUS_SIM_PORT", "5020"))
    unit_id = int(os.environ.get("MODBUS_SIM_UNIT_ID", "1"))
    try:
        return 0 if check("127.0.0.1", port, unit_id) else 1
    except OSError:
        return 1


if __name__ == "__main__":
    sys.exit(main())
