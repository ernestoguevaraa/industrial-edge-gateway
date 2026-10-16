"""Runs the real asyncua server on a free local port and talks to it as a client."""

import asyncio
import socket
from datetime import datetime, timedelta, timezone

import pytest
from asyncua import Client, ua

import plant_simulator.server as server_module
from plant_simulator.nodes import NAMESPACE_URI, NODE_SPECS, ValueType
from plant_simulator.server import NamespaceIndexError, PlantServer
from plant_simulator.settings import Settings

EXPECTED_DATA_TYPES = {
    ValueType.STRING: ua.ObjectIds.String,
    ValueType.DOUBLE: ua.ObjectIds.Double,
    ValueType.UINT32: ua.ObjectIds.UInt32,
}


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture
async def running():
    settings = Settings(host="127.0.0.1", port=_free_port(), tick_ms=50).validate()
    server = PlantServer(settings)
    await server.start()
    client = Client(f"opc.tcp://127.0.0.1:{settings.port}/", timeout=5)
    await client.connect()
    try:
        yield server, client
    finally:
        await client.disconnect()
        await server.stop()


async def test_namespace_is_registered_at_index_2(running):
    server, client = running
    assert server.namespace_index == 2
    assert await client.get_namespace_index(NAMESPACE_URI) == 2


async def test_documented_nodes_can_be_browsed(running):
    _, client = running
    objects = client.nodes.objects
    for spec in NODE_SPECS:
        node = await objects.get_child(["2:Site01", "2:Line01", f"2:{spec.asset}", f"2:{spec.name}"])
        assert node.nodeid.to_string() == spec.node_id()


async def test_documented_nodes_can_be_read_with_types_status_and_timestamps(running):
    _, client = running
    now = datetime.now(timezone.utc)
    for spec in NODE_SPECS:
        node = client.get_node(spec.node_id())
        assert (await node.read_data_type()).Identifier == EXPECTED_DATA_TYPES[spec.value_type]
        data_value = await node.read_data_value()
        assert data_value.StatusCode.is_good()
        assert data_value.Value.VariantType.name == spec.value_type.value
        assert data_value.SourceTimestamp is not None
        assert data_value.ServerTimestamp is not None
        source = data_value.SourceTimestamp.replace(tzinfo=timezone.utc)
        assert abs(source - now) < timedelta(seconds=30)


async def test_engineering_units_property(running):
    _, client = running
    eu = await client.get_node("ns=2;s=Line01.Oven01.Temperature.EngineeringUnits").read_value()
    assert eu.DisplayName.Text == "degC"
    assert eu.UnitId == 4408652  # UNECE "CEL"


async def test_variables_are_read_only(running):
    _, client = running
    node = client.get_node("ns=2;s=Line01.Press01.ProductionCount")
    with pytest.raises(ua.UaStatusCodeError):
        await node.write_value(ua.Variant(99, ua.VariantType.UInt32))


async def test_tick_updates_values_and_source_timestamps(running):
    server, client = running
    pressure = client.get_node("ns=2;s=Line01.Press01.Pressure")
    count = client.get_node("ns=2;s=Line01.Press01.ProductionCount")
    before_pressure = await pressure.read_data_value()
    before_count = await count.read_data_value()

    for _ in range(40):  # 40 ticks of simulated time; no sleeping on the period
        await server.tick()

    after_pressure = await pressure.read_data_value()
    assert after_pressure.Value.Value != before_pressure.Value.Value
    assert after_pressure.SourceTimestamp >= before_pressure.SourceTimestamp
    after_count = await count.read_data_value()
    assert after_count.Value.Value == server.model.press.production_count
    assert after_count.Value.Value >= before_count.Value.Value


async def test_unchanged_value_keeps_its_source_timestamp(running):
    server, client = running
    # CycleTime stays 0.0 until the first stroke completes (> 2 s simulated time
    # into RUNNING); during the first ticks it is unchanged and must not be rewritten.
    cycle_time = client.get_node("ns=2;s=Line01.Press01.CycleTime")
    initial = await cycle_time.read_data_value()
    await asyncio.sleep(0.01)
    await server.tick()
    after = await cycle_time.read_data_value()
    assert after.Value.Value == initial.Value.Value == 0.0
    assert after.SourceTimestamp == initial.SourceTimestamp


async def test_bad_quality_is_exposed_as_status_code(running):
    server, client = running
    pressure = client.get_node("ns=2;s=Line01.Press01.Pressure")
    # Press loop: STARTING 5 + RUNNING 120 + IDLE 20 + RUNNING 90 = 235 s, then FAULT;
    # BadSensorFailure from 5 s into FAULT.
    while server.model.press.pressure_quality.value != "BadSensorFailure":
        await server.tick()
    data_value = await pressure.read_data_value(raise_on_bad_status=False)
    assert data_value.StatusCode.value == ua.StatusCodes.BadSensorFailure
    assert data_value.SourceTimestamp is not None


async def test_run_loop_ticks_and_stops_on_event(running):
    server, _ = running
    stop = asyncio.Event()
    task = asyncio.create_task(server.run(stop))
    await asyncio.sleep(0.3)
    stop.set()
    await asyncio.wait_for(task, timeout=2)
    assert server.model.tick >= 3


async def test_unexpected_namespace_index_fails_clearly(monkeypatch):
    monkeypatch.setattr(server_module, "EXPECTED_NAMESPACE_INDEX", 3)
    server = PlantServer(Settings(host="127.0.0.1", port=_free_port()))
    with pytest.raises(NamespaceIndexError, match="index 2.*ns=3"):
        await server.start()
