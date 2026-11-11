#!/usr/bin/env python3
"""Validates the generated contracts: OpenAPI 3.1 document and JSON Schema 2020-12 for every tool schema.

Usage: build/contracts-venv/bin/python api/tools/validate.py   (needs api/tools/requirements.txt)
"""
import json
import pathlib
import sys

import yaml
from jsonschema import Draft202012Validator
from openapi_spec_validator import validate

GEN = pathlib.Path(__file__).resolve().parents[1] / "generated"


def main() -> int:
    spec = yaml.safe_load((GEN / "openapi.yaml").read_text())
    validate(spec)
    manifest = json.loads((GEN / "tools.json").read_text())
    for tool in manifest["tools"]:
        for key in ("input_schema", "output_schema"):
            try:
                Draft202012Validator.check_schema(tool[key])
            except Exception as e:  # noqa: BLE001 - report which tool failed
                print(f"{tool['name']}.{key}: {e}", file=sys.stderr)
                return 1
    agent = json.loads((GEN / "agent_tools.json").read_text())
    print(f"valid: OpenAPI 3.1 ({len(spec['paths'])} paths), {len(manifest['tools'])} tool schemas, "
          f"{len(agent)} agent tools, contract {manifest['contract_version']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
