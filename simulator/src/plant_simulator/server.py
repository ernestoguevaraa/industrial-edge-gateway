"""asyncua wiring: address space, initial values and the fixed-period update loop.

This is the only module that depends on asyncua.
"""

from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timezone
from typing import Callable

from asyncua import Server, ua
from asyncua.common.node import Node

from plant_simulator.model import PlantModel, Sample, Snapshot
from plant_simulator.nodes import (
    ASSETS,
    EXPECTED_NAMESPACE_INDEX,
    LINE,
    NAMESPACE_URI,
    NODE_SPECS,
    SITE,
    NodeSpec,
    ValueType,
)
from plant_simulator.scenario import Quality
from plant_simulator.settings import Settings

log = logging.getLogger("plant_simulator.server")

_VARIANT_TYPES = {
    ValueType.STRING: ua.VariantType.String,
    ValueType.DOUBLE: ua.VariantType.Double,
    ValueType.UINT32: ua.VariantType.UInt32,
}

_STATUS_CODES = {
    Quality.GOOD: ua.StatusCodes.Good,
    Quality.UNCERTAIN_SENSOR_NOT_ACCURATE: ua.StatusCodes.UncertainSensorNotAccurate,
    Quality.BAD_SENSOR_FAILURE: ua.StatusCodes.BadSensorFailure,
}

# UNECE Recommendation 20 common codes, as required for EUInformation.
_UNECE_UNITS_URI = "http://www.opcfoundation.org/UA/units/un/cefact"
_UNECE_CODES = {"bar": "BAR", "s": "SEC", "degC": "CEL", "units": "C62"}


# Upper bound for asyncua's Server.stop(). asyncua 1.1.8 on Python 3.12 can wait forever in
# asyncio's Server.wait_closed() for a client that reconnects while the server is stopping.
# 3 s is well above a normal stop (~1 s) and below docker stop's default 10 s grace period.
GRACEFUL_SHUTDOWN_TIMEOUT_S = 3.0


class NamespaceIndexError(RuntimeError):
    pass


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _eu_information(unit: str) -> ua.EUInformation:
    unit_id = 0
    for char in _UNECE_CODES[unit]:
        unit_id = (unit_id << 8) | ord(char)
    return ua.EUInformation(
        NamespaceUri=_UNECE_UNITS_URI,
        UnitId=unit_id,
        DisplayName=ua.LocalizedText(unit),
        Description=ua.LocalizedText(unit),
    )


def make_data_value(spec: NodeSpec, sample: Sample, timestamp: datetime) -> ua.DataValue:
    """Build a DataValue with Value, StatusCode, SourceTimestamp and ServerTimestamp all set.

    SourceTimestamp is the time at which the simulator produced this DataValue,
    i.e. the wall-clock time of the tick that last wrote the variable.
    """
    return ua.DataValue(
        Value=ua.Variant(sample.value, _VARIANT_TYPES[spec.value_type]),
        StatusCode_=ua.StatusCode(_STATUS_CODES[sample.quality]),
        SourceTimestamp=timestamp,
        ServerTimestamp=timestamp,
    )


class PlantServer:
    def __init__(
        self,
        settings: Settings,
        *,
        model: PlantModel | None = None,
        clock: Callable[[], datetime] = utc_now,
    ) -> None:
        self._settings = settings
        self._model = model or PlantModel(
            settings.seed, settings.dt_s, quality_events=settings.quality_events
        )
        self._clock = clock
        self._server = Server()
        self._namespace_index: int | None = None
        self._nodes: dict[str, tuple[NodeSpec, Node]] = {}
        self._last_written: dict[str, Sample] = {}

    @property
    def namespace_index(self) -> int:
        if self._namespace_index is None:
            raise RuntimeError("server not started")
        return self._namespace_index

    @property
    def model(self) -> PlantModel:
        return self._model

    async def start(self) -> None:
        settings = self._settings
        await self._server.init()
        self._server.set_endpoint(settings.endpoint)
        self._server.set_server_name("Industrial Edge Gateway Plant Simulator")
        await self._server.set_application_uri(f"{NAMESPACE_URI}:server")
        self._server.set_security_policy([ua.SecurityPolicyType.NoSecurity])

        index = await self._server.register_namespace(NAMESPACE_URI)
        if index != EXPECTED_NAMESPACE_INDEX:
            raise NamespaceIndexError(
                f"namespace {NAMESPACE_URI!r} was registered at index {index}, but the MVP "
                f"gateway configuration contract requires ns={EXPECTED_NAMESPACE_INDEX}"
            )
        self._namespace_index = index

        await self._build_address_space(index)
        # Initial values are written before the endpoint opens, so no client can
        # ever observe a variable without a SourceTimestamp.
        await self._publish(self._model.snapshot())
        await self._server.start()

        log.info(
            "OPC UA server started",
            extra={
                "event": "server_started",
                "endpoint": settings.endpoint,
                "namespace_uri": NAMESPACE_URI,
                "namespace_index": index,
                "seed": settings.seed,
                "tick_ms": settings.tick_ms,
                "quality_events": settings.quality_events,
                "node_ids": [spec.node_id(index) for spec in NODE_SPECS],
            },
        )

    async def stop(self, timeout_s: float = GRACEFUL_SHUTDOWN_TIMEOUT_S) -> bool:
        """Stop the OPC UA server gracefully, bounded by ``timeout_s``.

        Returns False if graceful shutdown timed out; the caller then exits anyway. Only the
        timeout is handled here: any other error from asyncua propagates.
        """
        try:
            await asyncio.wait_for(self._server.stop(), timeout=timeout_s)
        except TimeoutError:
            log.error(
                "graceful OPC UA shutdown timed out; terminating without it",
                extra={"event": "shutdown_timeout", "timeout_s": timeout_s},
            )
            return False
        log.info("OPC UA server stopped", extra={"event": "server_stopped"})
        return True

    async def _build_address_space(self, ns: int) -> None:
        objects = self._server.nodes.objects
        site = await objects.add_object(ua.NodeId(SITE, ns), ua.QualifiedName(SITE, ns))
        line = await site.add_object(ua.NodeId(LINE, ns), ua.QualifiedName(LINE, ns))
        assets: dict[str, Node] = {}
        for asset in ASSETS:
            assets[asset] = await line.add_object(
                ua.NodeId(f"{LINE}.{asset}", ns), ua.QualifiedName(asset, ns)
            )

        initial = self._model.snapshot().samples
        for spec in NODE_SPECS:
            variable = await assets[spec.asset].add_variable(
                ua.NodeId(spec.identifier, ns),
                ua.QualifiedName(spec.name, ns),
                initial[spec.identifier].value,
                varianttype=_VARIANT_TYPES[spec.value_type],
            )
            if spec.unit:
                await variable.add_property(
                    ua.NodeId(f"{spec.identifier}.EngineeringUnits", ns),
                    ua.QualifiedName("EngineeringUnits", 0),
                    _eu_information(spec.unit),
                    datatype=ua.NodeId(ua.ObjectIds.EUInformation),
                )
            self._nodes[spec.identifier] = (spec, variable)

    async def tick(self) -> Snapshot:
        """Advance the model by one fixed dt and write what changed."""
        snapshot = self._model.step()
        await self._publish(snapshot)
        return snapshot

    async def _publish(self, snapshot: Snapshot) -> None:
        timestamp = self._clock()
        for identifier, sample in snapshot.samples.items():
            previous = self._last_written.get(identifier)
            if previous == sample:
                # Unchanged value and status: no write, so the node keeps the
                # SourceTimestamp of the tick that last updated it.
                continue
            spec, variable = self._nodes[identifier]
            await self._server.write_attribute_value(
                variable.nodeid, make_data_value(spec, sample, timestamp)
            )
            self._last_written[identifier] = sample
            if previous is not None:
                self._log_change(spec, previous, sample, snapshot)

    def _log_change(self, spec: NodeSpec, previous: Sample, sample: Sample, snapshot: Snapshot) -> None:
        if spec.name == "State" and previous.value != sample.value:
            log.info(
                "equipment state changed",
                extra={
                    "event": "state_transition",
                    "asset": spec.asset,
                    "from": previous.value,
                    "to": sample.value,
                    "sim_time_s": snapshot.sim_time_s,
                },
            )
        if previous.quality != sample.quality:
            log.info(
                "variable quality changed",
                extra={
                    "event": "quality_change",
                    "asset": spec.asset,
                    "tag": spec.name,
                    "from": previous.quality.value,
                    "to": sample.quality.value,
                    "sim_time_s": snapshot.sim_time_s,
                },
            )
        if log.isEnabledFor(logging.DEBUG):
            log.debug(
                "value written",
                extra={
                    "event": "value_written",
                    "asset": spec.asset,
                    "tag": spec.name,
                    "value": sample.value,
                    "quality": sample.quality.value,
                },
            )

    async def run(self, stop: asyncio.Event) -> None:
        """Tick at a fixed period until ``stop`` is set.

        Deadlines are scheduled on the monotonic clock. The model always advances
        by the fixed dt, so scheduling jitter never changes simulated values. If a
        tick overruns, the loop logs it and resynchronises instead of bursting.
        """
        loop = asyncio.get_running_loop()
        period = self._settings.dt_s
        deadline = loop.time() + period
        while not stop.is_set():
            remaining = deadline - loop.time()
            if remaining > 0:
                try:
                    await asyncio.wait_for(stop.wait(), timeout=remaining)
                    break
                except TimeoutError:
                    pass
            await self.tick()
            deadline += period
            lateness = loop.time() - deadline
            if lateness > 0:
                log.warning(
                    "tick overran its period; resynchronising",
                    extra={"event": "tick_overrun", "lateness_s": round(lateness, 3)},
                )
                deadline = loop.time() + period
