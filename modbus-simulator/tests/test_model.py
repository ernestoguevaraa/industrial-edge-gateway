"""The register model: documented values, encodings, script and determinism."""

import struct

import pytest

from modbus_simulator.model import (
    CYCLE_TICKS,
    RUN_TICKS,
    CompressorModel,
    f32_words,
    to_u16,
    u32_words,
)


def at_tick(tick: int, tick_ms: int = 1000) -> CompressorModel:
    model = CompressorModel(tick_ms)
    for _ in range(tick):
        model.advance()
    return model


def test_encodings_match_the_documented_register_values():
    assert to_u16(-125) == 0xFF83
    assert u32_words(100_000, high_word_first=True) == (0x0001, 0x86A0)
    assert u32_words(-3500, high_word_first=False) == (0xF254, 0xFFFF)
    assert f32_words(12.5, high_word_first=True) == (0x4148, 0x0000)
    assert f32_words(48.25, high_word_first=False) == (0x0000, 0x4241)


@pytest.mark.parametrize("bad", [-0x8001, 0x1_0000])
def test_to_u16_rejects_values_outside_16_bits(bad):
    with pytest.raises(ValueError):
        to_u16(bad)


def test_first_tick_serves_the_documented_running_values():
    tables = at_tick(0).tables()
    assert tables.coils == (True, False)
    assert tables.discrete_inputs == (True, False)
    assert tables.holding_registers == (700, 0xFF83, 0x0001, 0x86A0, 0x4148, 0x0000)
    assert tables.input_registers == (0xF254, 0xFFFF, 0x0000, 0x4241, 0)


def test_pressure_follows_a_ten_tick_sawtooth_while_running():
    pressures = [at_tick(n).tables().holding_registers[0] for n in range(12)]
    assert pressures == [700, 705, 710, 715, 720, 725, 730, 735, 740, 745, 700, 705]


def test_compressor_stops_after_the_run_phase_and_restarts():
    stopped = at_tick(RUN_TICKS).tables()
    assert stopped.coils[0] is False
    assert stopped.holding_registers[0] == 0
    assert stopped.holding_registers[4:6] == (0, 0)  # float32 0.0
    assert at_tick(CYCLE_TICKS).tables().coils[0] is True


def test_running_time_counts_running_seconds_only():
    def running_time(model: CompressorModel) -> int:
        high, low = model.tables().holding_registers[2:4]
        return (high << 16) | low

    assert running_time(at_tick(10)) == 100_010
    assert running_time(at_tick(RUN_TICKS)) == 100_050
    assert running_time(at_tick(CYCLE_TICKS)) == 100_050  # no time while stopped
    assert running_time(at_tick(10, tick_ms=500)) == 100_005


def test_cycle_count_increments_every_tick_and_wraps():
    assert at_tick(3).tables().input_registers[4] == 3
    model = CompressorModel()
    for _ in range(0x1_0000):
        model.advance()
    assert model.tables().input_registers[4] == 0


def test_constant_values_decode_as_documented():
    tables = at_tick(5).tables()
    (pressure_delta,) = struct.unpack(">i", struct.pack(">HH", *reversed(tables.input_registers[0:2])))
    (oil,) = struct.unpack(">f", struct.pack(">HH", *reversed(tables.input_registers[2:4])))
    (current,) = struct.unpack(">f", struct.pack(">HH", *tables.holding_registers[4:6]))
    assert pressure_delta == -3500
    assert oil == 48.25
    assert current == 12.5


def test_identical_models_produce_identical_sequences():
    first, second = CompressorModel(), CompressorModel()
    for _ in range(200):
        assert first.tables() == second.tables()
        first.advance()
        second.advance()


def test_rejects_a_non_positive_tick():
    with pytest.raises(ValueError):
        CompressorModel(0)
