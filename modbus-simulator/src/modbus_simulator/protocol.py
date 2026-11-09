"""Modbus TCP framing and request handling for function codes 01-04 (pure, no I/O).

Implements the subset of the Modbus Application Protocol V1.1b3 the gateway uses: Read Coils
(0x01), Read Discrete Inputs (0x02), Read Holding Registers (0x03) and Read Input Registers
(0x04), with standard exception responses.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from modbus_simulator.model import Tables

MBAP_SIZE = 7  # transaction id (2), protocol id (2), length (2), unit id (1)
MAX_PDU_SIZE = 253

READ_COILS = 0x01
READ_DISCRETE_INPUTS = 0x02
READ_HOLDING_REGISTERS = 0x03
READ_INPUT_REGISTERS = 0x04

ILLEGAL_FUNCTION = 0x01
ILLEGAL_DATA_ADDRESS = 0x02
ILLEGAL_DATA_VALUE = 0x03
GATEWAY_TARGET_FAILED = 0x0B  # used for requests to another unit ID

MAX_READ_BITS = 2000
MAX_READ_REGISTERS = 125


class FramingError(ValueError):
    """The byte stream is not Modbus TCP; the connection must be closed."""


@dataclass(frozen=True)
class Header:
    transaction_id: int
    protocol_id: int
    length: int
    unit_id: int

    @property
    def pdu_size(self) -> int:
        return self.length - 1


def parse_header(data: bytes) -> Header:
    if len(data) != MBAP_SIZE:
        raise FramingError(f"MBAP header needs {MBAP_SIZE} bytes, got {len(data)}")
    transaction_id, protocol_id, length, unit_id = struct.unpack(">HHHB", data)
    if protocol_id != 0:
        raise FramingError(f"protocol id {protocol_id} is not Modbus (0)")
    if not 2 <= length <= MAX_PDU_SIZE + 1:
        raise FramingError(f"MBAP length {length} out of range")
    return Header(transaction_id, protocol_id, length, unit_id)


def frame(header: Header, pdu: bytes) -> bytes:
    """The response frame for `header`'s request with the given response PDU."""
    return struct.pack(">HHHB", header.transaction_id, 0, len(pdu) + 1, header.unit_id) + pdu


def exception_pdu(function: int, code: int) -> bytes:
    return bytes((function | 0x80, code))


def _pack_bits(bits: tuple[bool, ...]) -> bytes:
    data = bytearray((len(bits) + 7) // 8)
    for index, bit in enumerate(bits):
        if bit:
            data[index // 8] |= 1 << (index % 8)
    return bytes(data)


def handle_pdu(unit_id: int, served_unit_id: int, pdu: bytes, tables: Tables) -> bytes:
    """The response PDU for one request PDU (an exception response for invalid requests)."""
    if not pdu:
        raise FramingError("empty PDU")
    function = pdu[0]
    if unit_id != served_unit_id:
        return exception_pdu(function, GATEWAY_TARGET_FAILED)
    sources = {
        READ_COILS: (tables.coils, MAX_READ_BITS),
        READ_DISCRETE_INPUTS: (tables.discrete_inputs, MAX_READ_BITS),
        READ_HOLDING_REGISTERS: (tables.holding_registers, MAX_READ_REGISTERS),
        READ_INPUT_REGISTERS: (tables.input_registers, MAX_READ_REGISTERS),
    }
    if function not in sources or len(pdu) != 5:
        return exception_pdu(function, ILLEGAL_FUNCTION)
    start, quantity = struct.unpack(">HH", pdu[1:5])
    table, limit = sources[function]
    if not 1 <= quantity <= limit:
        return exception_pdu(function, ILLEGAL_DATA_VALUE)
    if start + quantity > len(table):
        return exception_pdu(function, ILLEGAL_DATA_ADDRESS)
    values = table[start : start + quantity]
    if function in (READ_COILS, READ_DISCRETE_INPUTS):
        data = _pack_bits(values)
    else:
        data = struct.pack(f">{quantity}H", *values)
    return bytes((function, len(data))) + data
