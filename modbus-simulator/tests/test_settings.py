"""Settings from the environment and the command line."""

import pytest

from modbus_simulator.settings import Settings, SettingsError, load


def test_defaults():
    assert load([], {}) == Settings()
    assert Settings().port == 5020 and Settings().unit_id == 1


def test_environment_and_flags_override_defaults():
    env = {"MODBUS_SIM_PORT": "1502", "MODBUS_SIM_UNIT_ID": "7", "MODBUS_SIM_LOG_LEVEL": "debug"}
    settings = load(["--tick-ms", "250"], env)
    assert (settings.port, settings.unit_id, settings.tick_ms, settings.log_level) == (
        1502, 7, 250, "DEBUG")
    assert load(["--port", "2000"], env).port == 2000


@pytest.mark.parametrize(
    ("argv", "env"),
    [
        ([], {"MODBUS_SIM_PORT": "abc"}),
        (["--unit-id", "0"], {}),
        (["--unit-id", "248"], {}),
        (["--tick-ms", "50"], {}),
        (["--port", "70000"], {}),
        (["--log-format", "xml"], {}),
        (["--unknown"], {}),
    ],
)
def test_rejects_invalid_settings(argv, env):
    with pytest.raises(SettingsError):
        load(argv, env)
