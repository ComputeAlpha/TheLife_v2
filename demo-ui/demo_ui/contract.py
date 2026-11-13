"""Reads api/generated/openapi.yaml and describes the resources and actions a user can reach at the
configured stage. The pages are generated from this, so a new contract resource appears without UI code."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

PAGING_PARAMS = {"limit", "cursor", "updated_since"}


@dataclass
class Operation:
    op_id: str
    method: str
    path: str
    summary: str
    body: dict | None  # request body schema (dereferenced at the top level)
    response: dict | None  # success response schema
    if_match: bool
    params: list[dict] = field(default_factory=list)

    @property
    def verb(self) -> str:
        return self.op_id.split(".", 1)[1]

    @property
    def returns_secret(self) -> bool:
        """Responses carrying a token are shown once and never stored by the UI."""
        return bool(self.response and "token" in self.response.get("properties", {}))


@dataclass
class Resource:
    prefix: str
    tag: str
    label: str
    description: str
    collection: str
    item: str
    schema: dict
    list_op: Operation
    get_op: Operation | None = None
    create: Operation | None = None
    update: Operation | None = None
    delete: Operation | None = None
    actions: list[Operation] = field(default_factory=list)  # POST {item}:verb
    reads: list[Operation] = field(default_factory=list)  # GET {item}/sub

    @property
    def filters(self) -> list[dict]:
        return [p for p in self.list_op.params if p["name"] not in PAGING_PARAMS]

    def title_of(self, item: dict) -> str:
        for k in ("title", "name", "summary", "statement"):
            if item.get(k):
                return str(item[k])
        return str(item.get("id", ""))


class Contract:
    def __init__(self, path: Path, stage: int):
        self.doc: dict = yaml.safe_load(Path(path).read_text())
        self.stage = stage
        self.schemas: dict = self.doc["components"]["schemas"]
        self.ops: dict[str, Operation] = {}
        for p, methods in self.doc["paths"].items():
            for m, o in methods.items():
                if m not in ("get", "post", "patch", "put", "delete") or not self._reachable(o):
                    continue
                self.ops[o["operationId"]] = self._operation(m, p, o)
        self.resources: dict[str, Resource] = self._resources()

    # ------------------------------------------------------------------ schemas
    def deref(self, s: dict | None) -> dict:
        """Follows a top-level $ref; sibling keys (default, description) override the target's."""
        if not s:
            return {}
        seen = 0
        while "$ref" in s and seen < 10:
            target = self.schemas[s["$ref"].rsplit("/", 1)[1]]
            s = {**target, **{k: v for k, v in s.items() if k != "$ref"}}
            seen += 1
        return s

    def _reachable(self, o: dict) -> bool:
        t = o.get("x-thelife-tool")
        if not t:
            return False
        return t.get("stage", 99) <= self.stage and "user" in t.get("principals", [])

    def _operation(self, method: str, path: str, o: dict) -> Operation:
        content = o.get("requestBody", {}).get("content", {})
        body = next(iter(content.values()), {}).get("schema") if content else None  # json or merge-patch+json
        response = None
        for code, r in o.get("responses", {}).items():
            if str(code).startswith("2") and "content" in r:
                response = self.deref(r["content"].get("application/json", {}).get("schema"))
                break
        params = []
        if_match = False
        for prm in o.get("parameters", []):
            if "$ref" in prm:
                name = prm["$ref"].rsplit("/", 1)[1]
                if_match |= name == "IfMatch"
                continue
            if prm.get("in") == "query":
                params.append({**prm, "schema": self.deref(prm.get("schema"))})
        return Operation(o["operationId"], method.upper(), path, o.get("summary", ""),
                         self.deref(body) if body else None, response, if_match, params)

    def _resources(self) -> dict[str, Resource]:
        tags = {}
        for t in self.doc.get("tags", []):
            tags.setdefault(t["name"], t.get("description", ""))
        out: dict[str, Resource] = {}
        for op in self.ops.values():
            if op.verb != "list" or op.method != "GET":
                continue
            prefix = op.op_id.split(".")[0]
            item_path = op.path + "/{id}"
            get_op = self._find(prefix, "GET", item_path)
            schema = self.deref(get_op.response if get_op else {})
            o = self.doc["paths"][op.path]["get"]
            label = re.sub(r"^List ", "", op.summary)
            r = Resource(prefix=prefix, tag=o["tags"][0], label=label[:1].upper() + label[1:],
                         description=self._schema_description(schema), collection=op.path, item=item_path,
                         schema=schema, list_op=op, get_op=get_op)
            r.create = self._find(prefix, "POST", op.path)
            r.update = self._find(prefix, "PATCH", item_path)
            r.delete = self._find(prefix, "DELETE", item_path)
            for other in self.ops.values():
                if not other.op_id.startswith(prefix + ".") or not other.path.startswith(item_path):
                    continue
                rest = other.path[len(item_path):]
                if other.method == "POST" and re.fullmatch(r":[a-z_]+", rest):
                    r.actions.append(other)
                elif other.method == "GET" and re.fullmatch(r"/[a-z_]+", rest):
                    r.reads.append(other)
            out[prefix] = r
        return dict(sorted(out.items(), key=lambda kv: (kv[1].tag, kv[1].label)))

    def _find(self, prefix: str, method: str, path: str) -> Operation | None:
        for op in self.ops.values():
            if op.op_id.startswith(prefix + ".") and op.method == method and op.path == path:
                return op
        return None

    @staticmethod
    def _schema_description(schema: dict) -> str:
        return str(schema.get("description", ""))

    def nav(self) -> list[tuple[str, list[Resource]]]:
        groups: dict[str, list[Resource]] = {}
        for r in self.resources.values():
            groups.setdefault(r.tag, []).append(r)
        return list(groups.items())

    def api_key_scopes(self) -> list[str]:
        """Scopes of tools an API key may call: the choices offered when issuing a key. The server
        enforces the same rule; this only populates the form."""
        out = set()
        for methods in self.doc["paths"].values():
            for m, o in methods.items():
                t = o.get("x-thelife-tool") if isinstance(o, dict) else None
                if t and "api_key" in t.get("principals", []):
                    out.update(t.get("scopes", []))
        return sorted(out)
