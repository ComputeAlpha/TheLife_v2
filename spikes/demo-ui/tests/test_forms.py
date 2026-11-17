from zoneinfo import ZoneInfo

import pytest
from werkzeug.datastructures import MultiDict

from demo_ui.forms import FormError, build_fields, to_body

TZ = ZoneInfo("Europe/Berlin")
SCHEMA = {
    "required": ["title"],
    "properties": {
        "title": {"type": "string", "maxLength": 200},
        "notes": {"type": ["string", "null"], "maxLength": 10000},
        "due_at": {"type": ["string", "null"], "format": "date-time"},
        "priority": {"type": "integer", "default": 1},
        "status": {"type": "string", "enum": ["open", "done"], "default": "open"},
        "meta": {"type": ["object", "null"]},
        "track": {"type": "boolean", "default": False},
        "workdays": {"type": "array", "items": {"type": "string", "enum": ["MO", "TU"]}},
        "day_profiles": {"type": "array", "items": {"type": "string"}},
    },
}


def fields(values=None):
    return build_fields(SCHEMA, lambda s: s, TZ, values=values)


def test_kinds_and_defaults():
    by = {f.name: f for f in fields()}
    assert [by[n].kind for n in SCHEMA["properties"]] == [
        "text", "textarea", "datetime", "number", "select", "json", "checkbox", "multiselect", "csv"]
    assert by["title"].required and not by["notes"].required
    assert by["status"].value == "open" and by["priority"].value == 1


def test_create_converts_local_time_and_types():
    body = to_body(fields(), MultiDict({"title": "Run", "due_at": "2026-10-25T09:00:00", "priority": "2",
                                        "status": "open", "meta": '{"a": 1}', "workdays": "MO",
                                        "day_profiles": "work, rest"}), TZ)
    # Berlin leaves DST on 2026-10-25: 09:00 local is UTC+1 after the switch
    assert body == {"title": "Run", "due_at": "2026-10-25T09:00:00+01:00", "priority": 2, "status": "open",
                    "meta": {"a": 1}, "track": False, "workdays": ["MO"], "day_profiles": ["work", "rest"]}


def test_update_sends_only_changes_and_clears_nullable():
    original = {"title": "Run", "notes": "x", "due_at": "2026-11-12T08:00:00+00:00", "priority": 1,
                "status": "open", "meta": None, "track": False, "workdays": ["MO"], "day_profiles": []}
    fs = fields(original)
    by = {f.name: f for f in fs}
    assert by["due_at"].value == "2026-11-12T09:00:00"  # shown in the user's timezone
    form = MultiDict({"title": "Run", "notes": "", "due_at": by["due_at"].value, "priority": "1", "status": "done",
                      "meta": "", "workdays": "MO", "day_profiles": ""})
    assert to_body(fs, form, TZ, original=original) == {"notes": None, "status": "done"}


def test_bad_json_is_a_field_error():
    with pytest.raises(FormError) as e:
        to_body(fields(), MultiDict({"title": "x", "meta": "{nope"}), TZ)
    assert "meta" in e.value.errors
