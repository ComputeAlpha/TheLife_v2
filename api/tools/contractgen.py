#!/usr/bin/env python3
"""Contract generator: one definition → OpenAPI, the C++ tool registry manifest, and agent tool definitions.

Sources (hand-written):
  api/common.yaml         shared schemas (Id, Area, Problem, ...)
  api/resources/*.yaml    one file per resource or endpoint group (fields, standard ops, custom actions)

Outputs (generated, committed, checked in CI):
  api/generated/openapi.yaml      public REST contract (OpenAPI 3.1)
  api/generated/tools.json        tool registry manifest embedded into the backend (effect, scopes, exposure,
                                  principals, target, input/output schemas with refs inlined, REST mapping)
  api/generated/agent_tools.json  OpenAI-format tool definitions for tools exposed to the agent

Usage:
  python3 api/tools/contractgen.py          regenerate
  python3 api/tools/contractgen.py --check  fail if generated files are stale or definitions are inconsistent

Format of the source files is documented in docs/tool_registry.md.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import sys

import yaml

API = pathlib.Path(__file__).resolve().parents[1]
GEN = API / "generated"
BASE = "/api/v1"

EFFECTS = {"read", "create", "update", "delete"}
EXPOSURES = {"rest", "agent", "mcp"}
PRINCIPALS = {"user", "agent", "api_key", "device"}
STANDARD_OPS = ("list", "get", "create", "update", "delete")
OP_EFFECT = {"list": "read", "get": "read", "create": "create", "update": "update", "delete": "delete"}
# Agent tool groups (docs/tool_registry.md §6): "core" is sent on every turn, the others on agent.load_tools.
AGENT_GROUPS = {"core", "planning", "work", "skills", "entertainment", "detections"}
POLICIES = {"user_reported_progress", "bulk_threshold", "user_stated_fact"}
TOOL_NAME = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")


class ContractError(Exception):
    pass


# ------------------------------------------------------------------ loading
def reject_bare_nulls(node, where):
    """An unquoted comma inside a flow mapping ({description: a, b}) silently becomes an extra key with a
    null value; no source value is legitimately null, so any null is treated as that mistake."""
    if isinstance(node, dict):
        for k, v in node.items():
            if v is None:
                raise SystemExit(f"{where}/{k}: null value (quote strings that contain commas)")
            reject_bare_nulls(v, f"{where}/{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            reject_bare_nulls(v, f"{where}/{i}")


def load_sources():
    common = yaml.safe_load((API / "common.yaml").read_text())
    reject_bare_nulls(common, "common.yaml")
    resources = []
    for f in sorted((API / "resources").glob("*.yaml")):
        doc = yaml.safe_load(f.read_text())
        reject_bare_nulls(doc, f.name)
        doc["_file"] = f.name
        resources.append(doc)
    return common, resources


# ------------------------------------------------------------------ schema helpers
def walk_refs(node, fn):
    """Returns a copy of node with every {"$ref": name, ...siblings} replaced by fn(name) merged with siblings."""
    if isinstance(node, dict):
        if "$ref" in node:
            siblings = {k: walk_refs(v, fn) for k, v in node.items() if k != "$ref"}
            out = fn(node["$ref"])
            return {**out, **siblings} if siblings else out
        return {k: walk_refs(v, fn) for k, v in node.items()}
    if isinstance(node, list):
        return [walk_refs(v, fn) for v in node]
    return node


def to_openapi(node):
    return walk_refs(node, lambda name: {"$ref": f"#/components/schemas/{name}"})


def inline(node, schemas, stack=()):
    def resolve(name):
        if name in stack:
            raise ContractError(f"recursive schema reference: {' -> '.join(stack + (name,))}")
        if name not in schemas:
            raise ContractError(f"unknown schema reference: {name}")
        return inline(copy.deepcopy(schemas[name]), schemas, stack + (name,))
    return walk_refs(node, resolve)


def obj(properties, required=(), additional=False, **extra):
    s = {"type": "object", "properties": properties, "additionalProperties": additional}
    if required:
        s["required"] = list(required)
    s.update(extra)
    return s


# ------------------------------------------------------------------ model building
class Model:
    def __init__(self, common, resources):
        self.schemas: dict = dict(common["schemas"])
        self.parameters: dict = common.get("parameters", {})
        self.tools: list[dict] = []
        self.operations: list[dict] = []  # (path, method, operation object)
        self.tags: list[dict] = []
        # Schemas first, so any resource may reference another's schemas regardless of file order.
        for r in resources:
            self.add_schemas(r)
        for r in resources:
            self.add_resource(r)
        self.validate()

    # --- resources
    def add_schemas(self, r):
        for sname, s in (r.get("extra_schemas") or {}).items():
            self._add_schema(sname, s, r)
        if r.get("schema"):
            self._add_resource_schemas(r)

    def add_resource(self, r):
        name = r["name"]
        if not any(t["name"] == r.get("tag", name) for t in self.tags):
            self.tags.append({"name": r.get("tag", name), "description": r.get("description", "").strip()})
        for op, overrides in (r.get("ops") or {}).items():
            if op not in STANDARD_OPS:
                raise ContractError(f"{r['_file']}: unknown standard op {op}")
            self._add_standard_op(r, op, overrides or {})
        for action in r.get("actions") or []:
            self._add_action(r, action)

    def _add_schema(self, name, s, r):
        if name in self.schemas:
            raise ContractError(f"{r['_file']}: schema {name} defined twice")
        self.schemas[name] = s

    def _add_resource_schemas(self, r):
        s, fields = r["schema"], r.get("fields") or {}
        readonly = r.get("readonly") or {}
        required = r.get("required") or []
        for f in required:
            if f not in fields:
                raise ContractError(f"{r['_file']}: required field {f} not in fields")
        overlap = set(fields) & set(readonly)
        if overlap:
            raise ContractError(f"{r['_file']}: fields and readonly overlap: {overlap}")
        meta = copy.deepcopy(self.schemas["ResourceMeta"]["properties"])
        props = {**meta, **{k: v for k, v in readonly.items()}, **fields}
        self._add_schema(s, obj(props, required=list(self.schemas["ResourceMeta"]["required"]) + required,
                                additional=False, description=r.get("description", "").strip()), r)
        self._add_schema(f"{s}Create", obj(fields, required=required), r)
        # Merge-patch semantics: absent means unchanged, so defaults must not appear in the patch schema.
        patch_fields = {k: {kk: vv for kk, vv in v.items() if kk != "default"} for k, v in fields.items()}
        self._add_schema(f"{s}Patch", obj(patch_fields, minProperties=1), r)
        self._add_schema(f"{s}Page", obj({"items": {"type": "array", "items": {"$ref": s}},
                                          "next_cursor": {"type": ["string", "null"]}},
                                         required=["items", "next_cursor"]), r)

    # --- tool + operation construction
    def _tool_meta(self, r, defaults, overrides):
        meta = {
            "exposure": overrides.get("exposure", r.get("exposure", ["rest", "agent", "mcp"])),
            "principals": overrides.get("principals", r.get("principals", ["user", "agent", "api_key"])),
            "stage": overrides.get("stage", r.get("stage", 1)),
            "agent_group": overrides.get("agent_group", r.get("agent_group")),
        }
        meta.update(defaults)
        return meta

    def _add_standard_op(self, r, op, ov):
        s, path, scope = r["schema"], BASE + r["path"], r["scope"]
        item_path = f"{path}/{{id}}"
        id_param = {"id": {"$ref": "Id"}}
        effect = OP_EFFECT[op]
        noun = r.get("noun", s.lower())
        filters = r.get("filters") or {}
        common_list = {"limit": {"type": "integer", "minimum": 1, "maximum": 200, "default": 50},
                       "cursor": {"type": "string", "maxLength": 512},
                       "updated_since": {"$ref": "Timestamp"},
                       "include_deleted": {"type": "boolean", "default": False}}
        spec = {
            "list": dict(method="get", path=path, query={**common_list, **filters}, body=None,
                         returns=f"{s}Page", status=200, summary=f"List {r.get('plural_noun', noun + 's')}"),
            "get": dict(method="get", path=item_path, params=id_param, body=None, returns=s, status=200,
                        summary=f"Get a {noun}", target=True),
            "create": dict(method="post", path=path, body=f"{s}Create", returns=s, status=201,
                           summary=f"Create a {noun}"),
            "update": dict(method="patch", path=item_path, params=id_param, body=f"{s}Patch", returns=s,
                           status=200, summary=f"Update a {noun}", target=True, if_match=True),
            "delete": dict(method="delete", path=item_path, params=id_param, body=None, returns=None, status=204,
                           summary=f"Delete a {noun} (soft delete)", target=True, if_match=True),
        }[op]
        spec.update({k: v for k, v in ov.items() if k in ("summary", "description")})
        spec["name"] = ov.get("tool", f"{r['name']}.{op}")
        spec["effect"] = effect
        spec["scopes"] = [f"{scope}:{'read' if effect == 'read' else 'write'}"]
        spec["description"] = ov.get("description", spec["summary"] + ".")
        self._emit(r, spec, ov)

    def _add_action(self, r, a):
        required_keys = {"name", "effect", "summary"}
        if "rest" in a.get("exposure", r.get("exposure", ["rest"])):
            required_keys |= {"method", "path"}
        missing = required_keys - set(a)
        if missing:
            raise ContractError(f"{r['_file']}: action {a.get('name')} missing {missing}")
        spec = dict(a)
        spec["name"] = a["name"] if "." in a["name"] else f"{r['name']}.{a['name']}"
        spec.setdefault("scopes", [f"{r['scope']}:{'read' if a['effect'] == 'read' else 'write'}"]
                        if r.get("scope") else [])
        spec.setdefault("description", a["summary"] + ".")
        spec.setdefault("status", 200)
        spec.setdefault("method", "post")
        spec["path"] = a.get("path", "") if a.get("absolute") else BASE + a.get("path", "")
        self._emit(r, spec, a)

    def _emit(self, r, spec, ov):
        name = spec["name"]
        if not TOOL_NAME.match(name):
            raise ContractError(f"{r['_file']}: bad tool name {name}")
        meta = self._tool_meta(r, {}, ov)
        params = spec.get("params") or {}
        query = spec.get("query") or {}
        body = spec.get("body")
        body_schema = {"$ref": body} if isinstance(body, str) else body
        body_props = {}
        body_required = []
        if body_schema is not None:
            resolved = inline(body_schema, self.schemas)
            body_props = resolved.get("properties", {})
            body_required = resolved.get("required", [])
        for group in (params, query):
            clash = set(group) & set(body_props)
            if clash:
                raise ContractError(f"{name}: argument name clash between path/query and body: {clash}")

        # ---- tool manifest entry (args = path ∪ query ∪ body properties, refs inlined)
        if ov.get("tool") is not False and spec.get("tool", True) is not False:
            arg_props = {**params, **query}
            if body_props:
                arg_props.update(body_props)
            # Every update/delete of a target takes expected_version (docs/tool_registry.md §3): REST sends it as
            # If-Match; approvals pin it so a changed target makes the approval stale.
            if spec.get("if_match") or (spec["effect"] in ("update", "delete") and spec.get("target")):
                arg_props["expected_version"] = {"type": "integer", "minimum": 1, "description":
                    "Optimistic concurrency check. Required as If-Match over REST; optional for the agent."}
            input_schema = inline(obj(arg_props, required=list(params) + body_required), self.schemas)
            returns = spec.get("returns")
            output_schema = inline({"$ref": returns} if isinstance(returns, str) else (returns or {"type": "null"}),
                                   self.schemas)
            target = None
            if spec.get("target"):
                target = {"resource": r.get("schema") or spec.get("target_resource"), "id_arg": "id"}
            self.tools.append({
                "name": name,
                "description": spec["description"].strip(),
                "effect": spec["effect"],
                "scopes": spec["scopes"],
                "exposure": meta["exposure"],
                "principals": meta["principals"],
                "stage": meta["stage"],
                "target": target,
                "policy": spec.get("policy"),
                "agent_group": meta["agent_group"] if "agent" in meta["exposure"] else None,
                "input_schema": input_schema,
                "output_schema": output_schema,
                "rest": {"method": spec["method"].upper(), "path": spec["path"], "success_status": spec["status"],
                         "path_args": list(params), "query_args": list(query),
                         "body_args": list(body_props), "if_match": bool(spec.get("if_match")),
                         "content_type": spec.get("content_type")} if "rest" in meta["exposure"] else None,
            })

        # ---- OpenAPI operation
        if "rest" not in meta["exposure"]:
            return
        op = {"operationId": name, "tags": [r.get("tag", r["name"])], "summary": spec["summary"],
              "description": spec["description"].strip(), "parameters": [], "responses": {}}
        if spec.get("public"):
            op["security"] = []
        for pname, pschema in params.items():
            op["parameters"].append({"name": pname, "in": "path", "required": True, "schema": to_openapi(pschema)})
        for qname, qschema in query.items():
            op["parameters"].append({"name": qname, "in": "query", "required": False,
                                     "schema": to_openapi(qschema)})
        if spec.get("if_match"):
            op["parameters"].append({"$ref": "#/components/parameters/IfMatch"})
        returns_secret = "token" in inline({"$ref": spec["returns"]} if isinstance(spec.get("returns"), str)
                                           else (spec.get("returns") or {}), self.schemas).get("properties", {})
        if spec["method"].lower() == "post" and not returns_secret:  # secrets are never stored for replay
            op["parameters"].append({"$ref": "#/components/parameters/" + (
                "IdempotencyKeyRequired" if spec.get("idempotency_required") else "IdempotencyKey")})
        if not op["parameters"]:
            del op["parameters"]
        if body_schema is not None:
            ctype = spec.get("content_type") or ("application/merge-patch+json" if spec["method"].lower() == "patch"
                                                 else "application/json")
            op["requestBody"] = {"required": True, "content": {ctype: {"schema": to_openapi(body_schema)}}}
        status = str(spec["status"])
        returns = spec.get("returns")
        if returns is None:
            op["responses"][status] = {"description": "Done"}
        else:
            resp = {"description": "OK", "content": {spec.get("response_content_type", "application/json"): {
                "schema": to_openapi({"$ref": returns} if isinstance(returns, str) else returns)}}}
            if spec.get("target") or spec["effect"] in ("create", "update"):
                resp["headers"] = {"ETag": {"$ref": "#/components/headers/ETag"}}
            op["responses"][status] = resp
        if spec["effect"] != "read" and "api_key" in meta["principals"]:
            op["responses"]["202"] = {"$ref": "#/components/responses/Accepted"}
        for code, ref in (("401", "Unauthorized"), ("403", "Forbidden")):
            if not spec.get("public"):
                op["responses"][code] = {"$ref": f"#/components/responses/{ref}"}
        if spec.get("target") or params:
            op["responses"]["404"] = {"$ref": "#/components/responses/NotFound"}
        if body_schema is not None:
            op["responses"]["400"] = {"$ref": "#/components/responses/BadRequest"}
            op["responses"]["415"] = {"$ref": "#/components/responses/UnsupportedMediaType"}
        if body_schema is not None or query or params or spec.get("if_match") or op.get("parameters"):
            op["responses"]["422"] = {"$ref": "#/components/responses/ValidationFailed"}
        if spec["effect"] != "read" and not spec.get("public"):
            # unique values (23505) and state rules (TL409) on any write
            op["responses"]["409"] = {"$ref": "#/components/responses/Conflict"}
        if spec.get("if_match"):
            op["responses"]["412"] = {"$ref": "#/components/responses/PreconditionFailed"}
            op["responses"]["428"] = {"$ref": "#/components/responses/PreconditionRequired"}
        for code, ref in (spec.get("extra_responses") or {}).items():
            op["responses"][str(code)] = {"$ref": f"#/components/responses/{ref}"}
        op["x-thelife-tool"] = {"effect": spec["effect"], "scopes": spec["scopes"], "exposure": meta["exposure"],
                                "principals": meta["principals"], "stage": meta["stage"]}
        self.operations.append({"path": spec["path"], "method": spec["method"].lower(), "op": op})

    # --- checks
    def validate(self):
        grantable = sorted({sc for t in self.tools if "api_key" in t["principals"] for sc in t["scopes"]})
        declared = sorted(self.schemas.get("Scope", {}).get("enum", []))
        if declared != grantable:
            raise ContractError(f"common.yaml Scope enum must list exactly the scopes of api_key tools: {grantable}")
        names = [t["name"] for t in self.tools]
        dupes = {n for n in names if names.count(n) > 1}
        if dupes:
            raise ContractError(f"duplicate tool names: {dupes}")
        agent_names = [t["name"].replace(".", "_") for t in self.tools]
        if len(set(agent_names)) != len(agent_names):
            raise ContractError("tool names collide after '.'→'_' mapping for the agent")
        seen = set()
        for o in self.operations:
            key = (o["path"], o["method"])
            if key in seen:
                raise ContractError(f"duplicate operation {o['method'].upper()} {o['path']}")
            seen.add(key)
        for t in self.tools:
            if t["effect"] not in EFFECTS:
                raise ContractError(f"{t['name']}: bad effect {t['effect']}")
            if not set(t["exposure"]) <= EXPOSURES or not set(t["principals"]) <= PRINCIPALS:
                raise ContractError(f"{t['name']}: bad exposure/principals")
            if "device" in t["principals"] and ("agent" in t["exposure"] or "mcp" in t["exposure"]):
                raise ContractError(f"{t['name']}: device tools must be REST-only")
            if t["effect"] != "read" and "agent" in t["exposure"] and t["effect"] in ("update", "delete") \
                    and not t["target"] and not t.get("policy"):
                raise ContractError(f"{t['name']}: agent-exposed {t['effect']} needs a target or explicit policy "
                                    f"so the permission policy can inspect provenance")
            if "agent" in t["exposure"] and t["agent_group"] not in AGENT_GROUPS:
                raise ContractError(f"{t['name']}: agent tools need agent_group, one of {sorted(AGENT_GROUPS)}")
            if t.get("policy") and t["policy"] not in POLICIES:
                raise ContractError(f"{t['name']}: unknown policy {t['policy']}")
            if t["rest"] and not t["scopes"] and "api_key" in t["principals"]:
                raise ContractError(f"{t['name']}: api_key principals need scopes")
            for p in t["rest"]["path_args"] if t["rest"] else []:
                if "{" + p + "}" not in t["rest"]["path"]:
                    raise ContractError(f"{t['name']}: path arg {p} not in path {t['rest']['path']}")
            if t["rest"]:
                in_path = set(re.findall(r"{(\w+)}", t["rest"]["path"]))
                if in_path != set(t["rest"]["path_args"]):
                    raise ContractError(f"{t['name']}: path placeholders {in_path} != params {t['rest']['path_args']}")

    # --- outputs
    def openapi(self, common):
        paths: dict = {}
        for o in sorted(self.operations, key=lambda x: (x["path"], x["method"])):
            paths.setdefault(o["path"], {})[o["method"]] = o["op"]
        return {
            "openapi": "3.1.0",
            "info": common["info"],
            "servers": [{"url": "/"}],
            "security": [{"bearer": []}],
            "tags": sorted(self.tags, key=lambda t: t["name"]),
            "paths": paths,
            "components": {
                "securitySchemes": {"bearer": {"type": "http", "scheme": "bearer", "description":
                    "Opaque token: tlu_ (user session), tld_ (device), tlk_ (API key)."}},
                "schemas": {k: to_openapi(v) for k, v in sorted(self.schemas.items())},
                "parameters": to_openapi(common["parameters"]),
                "headers": common["headers"],
                "responses": to_openapi(common["responses"]),
            },
        }

    def manifest(self):
        tools = sorted(self.tools, key=lambda t: t["name"])
        digest = hashlib.sha256(json.dumps(tools, sort_keys=True).encode()).hexdigest()[:16]
        groups: dict = {}
        for t in tools:
            if t.get("agent_group"):
                groups.setdefault(t["agent_group"], []).append(t["name"])
        return {"contract_version": digest, "agent_groups": groups, "tools": tools}

    def agent_tools(self):
        out = []
        for t in sorted(self.tools, key=lambda t: t["name"]):
            if "agent" not in t["exposure"]:
                continue
            out.append({"type": "function", "function": {
                "name": t["name"].replace(".", "_"),
                "description": t["description"],
                "parameters": t["input_schema"]}})
        return out


def dump_yaml(data) -> str:
    class Dumper(yaml.SafeDumper):
        pass
    Dumper.ignore_aliases = lambda *a: True
    return "# GENERATED by api/tools/contractgen.py. Do not edit; change api/common.yaml or api/resources/*.\n" + \
        yaml.dump(data, Dumper=Dumper, sort_keys=False, allow_unicode=True, width=120)


def dump_json(data) -> str:
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="fail if generated files are out of date")
    args = ap.parse_args()
    try:
        common, resources = load_sources()
        m = Model(common, resources)
    except ContractError as e:
        print(f"contract error: {e}", file=sys.stderr)
        return 1
    outputs = {
        GEN / "openapi.yaml": dump_yaml(m.openapi(common)),
        GEN / "tools.json": dump_json(m.manifest()),
        GEN / "agent_tools.json": dump_json(m.agent_tools()),
    }
    if args.check:
        stale = [p.name for p, text in outputs.items() if not p.exists() or p.read_text() != text]
        if stale:
            print(f"generated contracts are stale: {stale}. Run: python3 api/tools/contractgen.py",
                  file=sys.stderr)
            return 1
        print(f"contracts up to date: {len(m.tools)} tools, {len(m.operations)} operations")
        return 0
    GEN.mkdir(exist_ok=True)
    for p, text in outputs.items():
        p.write_text(text)
    print(f"generated {len(m.tools)} tools, {len(m.operations)} operations, {len(m.schemas)} schemas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
