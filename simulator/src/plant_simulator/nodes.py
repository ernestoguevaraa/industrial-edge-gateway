"""Single source of truth for the simulator's OPC UA address space.

This module is protocol-neutral on purpose (no asyncua import) so the node
contract can be tested without starting a server.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass

NAMESPACE_URI = "urn:industrial-edge-gateway:plant-simulator"

# The MVP gateway configuration (configs/gateway.example.yaml) hard-codes ns=2.
# The server registers NAMESPACE_URI and verifies the returned index equals this.
EXPECTED_NAMESPACE_INDEX = 2

SITE = "Site01"
LINE = "Line01"
PRESS = "Press01"
OVEN = "Oven01"


class ValueType(enum.Enum):
    STRING = "String"
    DOUBLE = "Double"
    UINT32 = "UInt32"


@dataclass(frozen=True)
class NodeSpec:
    asset: str
    name: str
    value_type: ValueType
    unit: str | None

    @property
    def identifier(self) -> str:
        """String NodeId identifier, e.g. ``Line01.Press01.Pressure``."""
        return f"{LINE}.{self.asset}.{self.name}"

    def node_id(self, namespace_index: int = EXPECTED_NAMESPACE_INDEX) -> str:
        return f"ns={namespace_index};s={self.identifier}"


NODE_SPECS: tuple[NodeSpec, ...] = (
    NodeSpec(PRESS, "State", ValueType.STRING, None),
    NodeSpec(PRESS, "Pressure", ValueType.DOUBLE, "bar"),
    NodeSpec(PRESS, "CycleTime", ValueType.DOUBLE, "s"),
    NodeSpec(PRESS, "ProductionCount", ValueType.UINT32, "units"),
    NodeSpec(OVEN, "State", ValueType.STRING, None),
    NodeSpec(OVEN, "Temperature", ValueType.DOUBLE, "degC"),
    NodeSpec(OVEN, "ProductionCount", ValueType.UINT32, "units"),
)

ASSETS: tuple[str, ...] = (PRESS, OVEN)
