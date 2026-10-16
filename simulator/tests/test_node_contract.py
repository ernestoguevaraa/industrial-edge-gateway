"""The simulator's address space must match the gateway configuration contract."""

from pathlib import Path

import yaml

from plant_simulator.nodes import EXPECTED_NAMESPACE_INDEX, NODE_SPECS

REPO_ROOT = Path(__file__).resolve().parents[2]
GATEWAY_CONFIG = REPO_ROOT / "configs" / "gateway.example.yaml"


def _configured_node_ids() -> set[str]:
    config = yaml.safe_load(GATEWAY_CONFIG.read_text(encoding="utf-8"))
    return {node["node_id"] for asset in config["assets"] for node in asset["nodes"]}


def test_simulator_exposes_exactly_the_configured_node_ids():
    simulated = {spec.node_id() for spec in NODE_SPECS}
    assert simulated == _configured_node_ids()


def test_contract_uses_namespace_index_2():
    assert EXPECTED_NAMESPACE_INDEX == 2
    assert all(node_id.startswith("ns=2;s=") for node_id in _configured_node_ids())


def test_units_match_gateway_config():
    config = yaml.safe_load(GATEWAY_CONFIG.read_text(encoding="utf-8"))
    configured = {
        node["node_id"]: node["unit"] for asset in config["assets"] for node in asset["nodes"]
    }
    for spec in NODE_SPECS:
        assert (spec.unit or "") == configured[spec.node_id()]
