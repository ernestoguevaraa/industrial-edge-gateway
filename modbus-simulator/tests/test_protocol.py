"""Modbus TCP framing and request handling (no I/O)."""

import struct

import pytest

from modbus_simulator.model import CompressorModel
from modbus_simulator.protocol import (
    FramingError,
    Header,
    frame,
    handle_pdu,
    parse_header,
)

TABLES = CompressorModel().tables()


def request(function: int, start: int, quantity: int) -> bytes:
    return struct.pack(">BHH", function, start, quantity)


def handle(function: int, start: int, quantity: int, unit: int = 1) -> bytes:
    return handle_pdu(unit, 1, request(function, start, quantity), TABLES)


def test_reads_holding_registers():
    assert handle(0x03, 0, 6) == bytes([0x03, 12]) + struct.pack(
        ">6H", 700, 0xFF83, 0x0001, 0x86A0, 0x4148, 0x0000
    )


def test_reads_input_registers():
    assert handle(0x04, 2, 2) == bytes([0x04, 4, 0x00, 0x00, 0x42, 0x41])


def test_reads_coils_and_discrete_inputs_as_packed_bits():
    assert handle(0x01, 0, 2) == bytes([0x01, 1, 0b01])  # running, not in maintenance
    assert handle(0x02, 0, 2) == bytes([0x02, 1, 0b01])  # door closed, oil ok


@pytest.mark.parametrize(
    ("function", "start", "quantity", "code"),
    [
        (0x03, 5, 2, 0x02),  # past the last holding register
        (0x04, 5, 1, 0x02),
        (0x01, 2, 1, 0x02),
        (0x03, 0, 0, 0x03),  # quantity 0
        (0x03, 0, 126, 0x03),  # above the protocol maximum
        (0x01, 0, 2001, 0x03),
    ],
)
def test_answers_invalid_requests_with_exceptions(function, start, quantity, code):
    assert handle(function, start, quantity) == bytes([function | 0x80, code])


def test_unsupported_function_is_illegal_function():
    assert handle_pdu(1, 1, bytes([0x06, 0, 0, 0, 1]), TABLES) == bytes([0x86, 0x01])


def test_other_unit_ids_get_gateway_target_exception():
    assert handle(0x03, 0, 1, unit=7) == bytes([0x83, 0x0B])


def test_parses_and_builds_mbap_frames():
    header = parse_header(struct.pack(">HHHB", 0x1234, 0, 6, 1))
    assert header == Header(transaction_id=0x1234, protocol_id=0, length=6, unit_id=1)
    assert header.pdu_size == 5
    assert frame(header, bytes([0x03, 2, 0x02, 0xBC])) == struct.pack(">HHHB", 0x1234, 0, 5, 1) + bytes(
        [0x03, 2, 0x02, 0xBC]
    )


@pytest.mark.parametrize(
    "raw",
    [
        struct.pack(">HHHB", 1, 1, 6, 1),  # not Modbus
        struct.pack(">HHHB", 1, 0, 1, 1),  # length too short
        struct.pack(">HHHB", 1, 0, 300, 1),  # length too long
        b"\x00\x01",
    ],
)
def test_rejects_malformed_headers(raw):
    with pytest.raises(FramingError):
        parse_header(raw)
