"""The UI is generated from the contract: every user-reachable Stage 1 CRUD resource gets pages."""
from pathlib import Path

import yaml

from demo_ui.contract import Contract

OPENAPI = Path(__file__).resolve().parents[2] / "api/generated/openapi.yaml"


def test_every_user_list_operation_becomes_a_resource():
    c = Contract(OPENAPI, 1)
    doc = yaml.safe_load(OPENAPI.read_text())
    expected = set()
    for methods in doc["paths"].values():
        for o in methods.values():
            t = o.get("x-thelife-tool", {})
            if o["operationId"].endswith(".list") and t.get("stage", 9) <= 1 and "user" in t.get("principals", []):
                expected.add(o["operationId"].split(".")[0])
    assert set(c.resources) == expected and len(expected) >= 17


def test_secret_returning_creates_and_actions_are_found():
    c = Contract(OPENAPI, 1)
    assert c.resources["devices"].create.returns_secret
    assert c.resources["api_keys"].create.returns_secret
    assert not c.resources["tasks"].create.returns_secret
    assert {a.verb for a in c.resources["detections"].actions} == {"confirm", "dispute", "claim"}
    assert c.resources["tasks"].update.if_match and c.resources["tasks"].delete.if_match
    assert c.resources["devices"].update is None and c.resources["devices"].delete.verb == "revoke"


def test_later_stage_and_device_only_operations_are_hidden():
    c = Contract(OPENAPI, 1)
    assert "devices.self_heartbeat" not in c.ops and "ingest.captures" not in c.ops


def test_stage_two_adds_approvals_and_conversations():
    c = Contract(OPENAPI, 2)
    assert {"approvals", "conversations"} <= set(c.resources)
    assert {a.verb for a in c.resources["approvals"].actions} == {"approve", "reject"}
    assert c.resources["approvals"].create is None and c.resources["approvals"].update is None
    assert "approvals" not in Contract(OPENAPI, 1).resources
    assert "devices.self_notifications" not in c.ops  # device-only


def test_memory_facts_are_listed_with_their_statement():
    c = Contract(OPENAPI, 2)
    r = c.resources["memory_facts"]
    assert r.create is None and r.update is None and r.delete is not None and r.delete.if_match
    assert r.title_of({"id": "x", "statement": "Prefers deep work before noon"}) == "Prefers deep work before noon"
