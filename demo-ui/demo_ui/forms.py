"""HTML form fields generated from contract schemas, and the reverse mapping from submitted form values to
a JSON body. Only presentation conversions happen here (local time <-> RFC 3339, text <-> numbers/JSON);
validation is the server's job and its 422 messages are shown next to the fields."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import date, datetime
from typing import Any, Callable, Mapping
from zoneinfo import ZoneInfo

# uuid fields that point at another resource: rendered as a select of that resource's items.
REFERENCES = {
    "goal_id": "goals",
    "parent_id": "goals",
    "project_id": "work_projects",
    "role_id": "work_roles",
    "resource_id": "skill_resources",
    "section_id": "skill_sections",
    "item_id": "entertainment_items",
    "detection_id": "detections",
}


class FormError(Exception):
    def __init__(self, errors: dict[str, str]):
        super().__init__(errors)
        self.errors = errors


@dataclass
class Field:
    name: str
    kind: str  # text textarea select multiselect number checkbox datetime date json
    required: bool
    nullable: bool
    help: str = ""
    options: list[tuple[str, str]] = field(default_factory=list)
    value: Any = ""  # form representation
    step: str = "1"
    error: str = ""


def types_of(s: Mapping) -> set[str]:
    t = s.get("type", "string")
    return set(t) if isinstance(t, list) else {t}


def kind_of(name: str, s: Mapping) -> str:
    t = types_of(s)
    if "object" in t:
        return "json"
    if "array" in t:
        return "multiselect" if s.get("items", {}).get("enum") else "csv"
    if s.get("enum"):
        return "select"
    if "boolean" in t:
        return "checkbox"
    if t & {"integer", "number"}:
        return "number"
    if s.get("format") == "date-time":
        return "datetime"
    if s.get("format") == "date":
        return "date"
    if s.get("maxLength", 0) > 500:
        return "textarea"
    return "text"


# ------------------------------------------------------------------ API value -> form value
def to_local_input(value: str | None, tz: ZoneInfo) -> str:
    if not value:
        return ""
    return datetime.fromisoformat(value).astimezone(tz).replace(tzinfo=None).isoformat(timespec="seconds")


def to_form(kind: str, value: Any, tz: ZoneInfo) -> Any:
    if value is None:
        return [] if kind == "multiselect" else ""
    if kind == "datetime":
        return to_local_input(value, tz)
    if kind == "json":
        return json.dumps(value, indent=2, ensure_ascii=False)
    if kind == "csv":
        return ", ".join(str(v) for v in value)
    if kind == "checkbox":
        return bool(value)
    return value


def build_fields(schema: Mapping, deref: Callable[[Any], dict], tz: ZoneInfo, *, values: Mapping | None = None,
                 options_for: Callable[[str, dict], list[tuple[str, str]]] | None = None,
                 required_override: bool | None = None) -> list[Field]:
    required = set(schema.get("required", []))
    out = []
    for name, raw in schema.get("properties", {}).items():
        s = deref(raw)
        kind = kind_of(name, s)
        nullable = "null" in types_of(s) or None in (s.get("enum") or [])
        f = Field(name=name, kind=kind, required=name in required if required_override is None else required_override,
                  nullable=nullable, help=str(s.get("description", "")))
        if kind == "select":
            f.options = [(str(v), str(v)) for v in s["enum"] if v is not None]
        elif kind == "multiselect":
            f.options = [(str(v), str(v)) for v in s["items"]["enum"]]
        elif kind == "number" and "integer" not in types_of(s):
            f.step = "any"
        if options_for:
            extra = options_for(name, s)
            if extra:
                f.kind, f.options = ("multiselect" if kind in ("multiselect", "csv") else "select"), extra
        if values is not None and name in values:
            f.value = to_form(kind, values[name], tz)
            if f.kind == "select" and f.value != "" and f.value not in {v for v, _ in f.options}:
                f.options.append((str(f.value), str(f.value)))
        elif "default" in s:
            f.value = to_form(kind, s["default"], tz)
        elif kind == "multiselect":
            f.value = []
        out.append(f)
    return out


# ------------------------------------------------------------------ form value -> API value
def from_local_input(text: str, tz: ZoneInfo) -> str:
    dt = datetime.fromisoformat(text)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=tz)
    return dt.isoformat()


def parse_value(f: Field, form: Mapping, tz: ZoneInfo) -> tuple[bool, Any]:
    """(present, value). Blank inputs are absent; the caller decides whether that means omit or null."""
    if f.kind == "checkbox":
        return True, form.get(f.name) == "on"
    if f.kind == "multiselect":
        values = form.getlist(f.name) if hasattr(form, "getlist") else form.get(f.name, [])
        return True, list(values)
    text = str(form.get(f.name, "")).strip()
    if text == "":
        return False, None
    try:
        if f.kind == "number":
            return True, int(text) if f.step == "1" else float(text)
        if f.kind == "datetime":
            return True, from_local_input(text, tz)
        if f.kind == "date":
            return True, date.fromisoformat(text).isoformat()
        if f.kind == "json":
            return True, json.loads(text)
        if f.kind == "csv":
            return True, [p.strip() for p in text.split(",") if p.strip()]
    except ValueError as e:
        raise FormError({f.name: f"not a valid {f.kind}: {e}"}) from None
    return True, text


def same(kind: str, a: Any, b: Any) -> bool:
    if kind == "datetime" and a and b:
        return datetime.fromisoformat(a) == datetime.fromisoformat(b)
    return a == b


def to_body(fields: list[Field], form: Mapping, tz: ZoneInfo, original: Mapping | None = None) -> dict:
    """Create (original None): every filled field. Update: only fields that changed (JSON Merge Patch); a
    cleared nullable field becomes null."""
    body: dict = {}
    errors: dict[str, str] = {}
    for f in fields:
        try:
            present, value = parse_value(f, form, tz)
        except FormError as e:
            errors.update(e.errors)
            continue
        if original is None:
            if present and not (f.kind == "multiselect" and not value and not f.required):
                body[f.name] = value
            continue
        if not present:
            if original.get(f.name) is not None and f.nullable:
                body[f.name] = None
            continue
        if not same(f.kind, value, original.get(f.name)):
            body[f.name] = value
    if errors:
        raise FormError(errors)
    return body


def keep_submitted(fields: list[Field], form: Mapping) -> None:
    """Re-render after an error with what the user typed."""
    for f in fields:
        if f.kind == "checkbox":
            f.value = form.get(f.name) == "on"
        elif f.kind == "multiselect":
            f.value = form.getlist(f.name) if hasattr(form, "getlist") else form.get(f.name, [])
        else:
            f.value = form.get(f.name, "")
