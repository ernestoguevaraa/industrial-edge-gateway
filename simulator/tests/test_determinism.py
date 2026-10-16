from plant_simulator.model import PlantModel, PressModel

TICKS = 10_000


def _sequence(seed: int, dt: float = 0.5, ticks: int = TICKS):
    model = PlantModel(seed=seed, dt=dt)
    return [dict(model.step().samples) for _ in range(ticks)]


def test_same_seed_produces_identical_sequence():
    assert _sequence(42) == _sequence(42)


def test_different_seed_produces_different_sequence():
    assert _sequence(42) != _sequence(43)


def test_state_sequence_is_seed_independent():
    # Seeds change the noise, not the scripted operating scenario.
    states = lambda seq: [s["Line01.Press01.State"].value for s in seq]  # noqa: E731
    assert states(_sequence(1, ticks=2000)) == states(_sequence(2, ticks=2000))


def test_assets_use_independent_random_streams():
    # A press built alone matches the press inside the plant, so adding or
    # changing other equipment cannot perturb its values.
    plant = PlantModel(seed=7, dt=0.5)
    standalone = PressModel(seed=7, dt=0.5, quality_events=True)
    for _ in range(2000):
        plant.step()
        standalone.step()
        assert plant.press.samples() == standalone.samples()
