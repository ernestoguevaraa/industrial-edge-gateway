import pytest

from plant_simulator.settings import Settings, SettingsError, load


def test_defaults():
    settings = load([], env={})
    assert settings == Settings()
    assert settings.endpoint == "opc.tcp://0.0.0.0:4840/"
    assert settings.quality_events is True


def test_environment_and_flag_precedence():
    env = {"SIM_PORT": "4841", "SIM_SEED": "7", "SIM_QUALITY_EVENTS": "0", "SIM_LOG_FORMAT": "TEXT"}
    settings = load(["--seed", "9"], env=env)
    assert settings.port == 4841
    assert settings.seed == 9
    assert settings.quality_events is False
    assert settings.log_format == "text"


@pytest.mark.parametrize(
    "env",
    [
        {"SIM_PORT": "abc"},
        {"SIM_PORT": "70000"},
        {"SIM_TICK_MS": "0"},
        {"SIM_QUALITY_EVENTS": "maybe"},
        {"SIM_LOG_FORMAT": "xml"},
        {"SIM_LOG_LEVEL": "LOUD"},
    ],
)
def test_invalid_settings_fail_clearly(env):
    with pytest.raises(SettingsError):
        load([], env=env)
