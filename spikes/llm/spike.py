#!/usr/bin/env python3
"""Stage 0 spike: verify glm-5.3-flash:cloud capabilities through the OpenAI-compatible API.

Checks (prelim_spec.md §5.5, agent_lld.md §3):
  chat, streaming + usage, tool calling (single, parallel, round trip), structured output
  (json_schema), vision, vision + structured output (the real pipeline shape),
  reasoning on/off, and error response bodies (recorded as fixtures for the error classifier).

Only synthetic images are sent (drawn here with PIL); no real screen content leaves the machine.
Uses about 20 requests. Raw responses go to spikes/llm/out/ (git-ignored); the summary is printed
and written to out/results.json.

Usage: python3 spikes/llm/spike.py [--base-url http://127.0.0.1:11434/v1] [--model glm-5.3-flash:cloud]
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import pathlib
import time
import urllib.error
import urllib.request

OUT = pathlib.Path(__file__).parent / "out"


# ---------------------------------------------------------------- http helpers
class Client:
    def __init__(self, base_url: str, model: str, api_key: str = "ollama", timeout: float = 180):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.api_key = api_key
        self.timeout = timeout

    def _request(self, path: str, body: dict):
        req = urllib.request.Request(
            self.base_url + path,
            data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.api_key}"},
            method="POST",
        )
        return urllib.request.urlopen(req, timeout=self.timeout)

    def chat(self, **kw) -> tuple[int, dict | str, dict, float]:
        body = {"model": self.model, **kw}
        t0 = time.monotonic()
        try:
            with self._request("/chat/completions", body) as r:
                raw = r.read().decode()
                status, headers = r.status, dict(r.headers)
        except urllib.error.HTTPError as e:
            raw = e.read().decode(errors="replace")
            status, headers = e.code, dict(e.headers)
        dt = time.monotonic() - t0
        try:
            return status, json.loads(raw), headers, dt
        except json.JSONDecodeError:
            return status, raw, headers, dt

    def stream(self, **kw) -> tuple[int, list[dict], float, float]:
        body = {"model": self.model, "stream": True, **kw}
        t0 = time.monotonic()
        first = None
        chunks: list[dict] = []
        with self._request("/chat/completions", body) as r:
            status = r.status
            for line in r:
                line = line.decode().strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                if first is None:
                    first = time.monotonic() - t0
                chunks.append(json.loads(data))
        return status, chunks, first or 0.0, time.monotonic() - t0


# ---------------------------------------------------------------- synthetic screens
def synthetic_screen(kind: str) -> str:
    """Draws a fake desktop screen and returns a data URI (JPEG, like the collector will send)."""
    from PIL import Image, ImageDraw, ImageFont

    W, H = 1600, 900
    mono = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 18)
    sans = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20)
    big = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 34)
    img = Image.new("RGB", (W, H), (30, 30, 30))
    d = ImageDraw.Draw(img)

    if kind == "ide":
        d.rectangle([0, 0, W, 36], fill=(50, 50, 50))
        d.text((12, 8), "health.cc — TheLife_v2 — Visual Studio Code", font=sans, fill=(220, 220, 220))
        d.rectangle([0, 36, 260, H], fill=(37, 37, 38))
        for i, f in enumerate(["backend/", "  src/", "    http/", "      health.cc", "    db/", "  tests/"]):
            d.text((12, 50 + i * 26), f, font=mono, fill=(200, 200, 200))
        code = [
            '#include "http/health.h"',
            "",
            "void registerHealthRoutes() {",
            '  drogon::app().registerHandler("/healthz",',
            "    [](const HttpRequestPtr&) -> Task<HttpResponsePtr> {",
            '      body["status"] = "ok";',
            "      co_return HttpResponse::newHttpJsonResponse(body);",
            "    }, {drogon::Get});",
            "}",
        ]
        for i, line in enumerate(code):
            d.text((290, 60 + i * 28), f"{i + 1:3}  {line}", font=mono, fill=(156, 220, 254))
        d.rectangle([260, 640, W, H], fill=(24, 24, 24))
        d.text((280, 660), "$ ctest --preset dev   100% tests passed, 0 tests failed out of 5", font=mono,
               fill=(120, 220, 120))
    elif kind == "lecture":
        d.rectangle([0, 0, W, 50], fill=(255, 255, 255))
        d.text((20, 12), "Lecture 7: Eigenvalues and Eigenvectors — MIT 18.06 Linear Algebra - Chromium",
               font=sans, fill=(20, 20, 20))
        d.rectangle([80, 90, 1200, 720], fill=(0, 0, 0))
        d.text((300, 300), "A v = λ v", font=big, fill=(255, 255, 255))
        d.text((300, 360), "det(A − λI) = 0", font=big, fill=(255, 255, 255))
        d.rectangle([80, 720, 1200, 730], fill=(80, 80, 80))
        d.rectangle([80, 720, 600, 730], fill=(220, 30, 30))
        d.text((90, 740), "23:41 / 49:10", font=sans, fill=(220, 220, 220))
    elif kind == "game":
        for y in range(H):
            d.line([(0, y), (W, y)], fill=(40, 90 + y // 12, 160))
        d.rectangle([0, 600, W, H], fill=(60, 140, 60))
        d.text((40, 30), "HP ██████████░░  ROUND 3   SCORE 12,450", font=big, fill=(255, 240, 120))
        d.rectangle([700, 480, 760, 600], fill=(200, 50, 50))
        d.text((1300, 840), "Press ESC for menu", font=sans, fill=(255, 255, 255))
    else:
        raise ValueError(kind)

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=80)
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()


# ---------------------------------------------------------------- schemas and tools
DETECTION_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["segments"],
    "properties": {
        "segments": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["area", "category", "summary", "confidence"],
                "properties": {
                    "area": {"type": "string",
                             "enum": ["work", "skill", "fitness", "personal", "entertainment", "other"]},
                    "category": {"type": "string"},
                    "summary": {"type": "string", "maxLength": 200},
                    "confidence": {"type": "number", "minimum": 0, "maximum": 1},
                },
            },
        }
    },
}

TOOLS = [
    {"type": "function", "function": {
        "name": "tasks_create",
        "description": "Create a task for the user.",
        "parameters": {"type": "object", "additionalProperties": False, "required": ["title"],
                       "properties": {"title": {"type": "string"},
                                      "due_at": {"type": "string", "description": "RFC 3339 timestamp"},
                                      "priority": {"type": "integer", "minimum": 0, "maximum": 3}}}}},
    {"type": "function", "function": {
        "name": "activities_log",
        "description": "Log an activity the user already did.",
        "parameters": {"type": "object", "additionalProperties": False,
                       "required": ["area", "title", "minutes"],
                       "properties": {"area": {"type": "string",
                                               "enum": ["work", "skill", "fitness", "personal", "entertainment"]},
                                      "title": {"type": "string"},
                                      "minutes": {"type": "integer", "minimum": 1}}}}},
    {"type": "function", "function": {
        "name": "reminders_create",
        "description": "Remind the user about something at a time.",
        "parameters": {"type": "object", "additionalProperties": False, "required": ["text", "due_at"],
                       "properties": {"text": {"type": "string"}, "due_at": {"type": "string"}}}}},
]

SYSTEM = ("You are the TheLife assistant. Current local time: 2026-11-12T14:05:00+05:30. "
          "Capture anything the user reports as data using the tools, then reply briefly.")


def validate(instance, schema) -> list[str]:
    """Tiny JSON Schema subset validator (types, enum, required, additionalProperties, bounds)."""
    errs: list[str] = []

    def walk(x, s, path):
        t = s.get("type")
        tmap = {"object": dict, "array": list, "string": str, "integer": int, "number": (int, float),
                "boolean": bool}
        if t and not isinstance(x, tmap[t]):
            errs.append(f"{path}: expected {t}")
            return
        if "enum" in s and x not in s["enum"]:
            errs.append(f"{path}: {x!r} not in enum")
        if t == "object":
            for k in s.get("required", []):
                if k not in x:
                    errs.append(f"{path}.{k}: missing")
            props = s.get("properties", {})
            for k, v in x.items():
                if k in props:
                    walk(v, props[k], f"{path}.{k}")
                elif s.get("additionalProperties") is False:
                    errs.append(f"{path}.{k}: not allowed")
        if t == "array":
            for i, v in enumerate(x):
                walk(v, s.get("items", {}), f"{path}[{i}]")
        if t in ("number", "integer"):
            if "minimum" in s and x < s["minimum"]:
                errs.append(f"{path}: below minimum")
            if "maximum" in s and x > s["maximum"]:
                errs.append(f"{path}: above maximum")
        if t == "string" and "maxLength" in s and len(x) > s["maxLength"]:
            errs.append(f"{path}: too long")

    walk(instance, schema, "$")
    return errs


# ---------------------------------------------------------------- checks
class Spike:
    def __init__(self, client: Client):
        self.c = client
        self.results: list[dict] = []

    def record(self, name, ok, latency=None, usage=None, notes="", raw=None):
        self.results.append({"check": name, "ok": ok, "latency_s": round(latency or 0, 2),
                             "usage": usage, "notes": notes})
        if raw is not None:
            (OUT / f"{name}.json").write_text(json.dumps(raw, indent=2, ensure_ascii=False))
        print(f"[{'PASS' if ok else 'FAIL'}] {name:28} {latency or 0:6.2f}s  {notes}")

    def run(self, name, fn):
        try:
            fn()
        except Exception as e:  # keep going; a failed check is a finding
            self.record(name, False, notes=f"exception: {type(e).__name__}: {e}")

    # --- basic
    def chat_basic(self):
        s, r, h, dt = self.c.chat(messages=[{"role": "user", "content": "Reply with exactly: pong"}],
                                  max_tokens=200)
        msg = r["choices"][0]["message"] if s == 200 else {}
        self.record("chat_basic", s == 200 and "pong" in (msg.get("content") or "").lower(), dt,
                    r.get("usage") if isinstance(r, dict) else None,
                    f"status={s} reasoning_field={'reasoning' in msg} content={msg.get('content')!r}", r)

    def reasoning_off(self):
        s, r, h, dt = self.c.chat(messages=[{"role": "user", "content": "Reply with exactly: pong"}],
                                  reasoning_effort="none", max_tokens=200)
        msg = r["choices"][0]["message"] if s == 200 else {}
        self.record("reasoning_off", s == 200, dt, r.get("usage") if isinstance(r, dict) else None,
                    f"status={s} reasoning_present={bool(msg.get('reasoning'))} content={msg.get('content')!r}",
                    r)

    def streaming(self):
        s, chunks, ttft, total = self.c.stream(
            messages=[{"role": "user", "content": "Count from 1 to 10, one number per line."}],
            stream_options={"include_usage": True}, reasoning_effort="none")
        text = "".join((c["choices"][0]["delta"].get("content") or "") for c in chunks if c.get("choices"))
        usage = next((c["usage"] for c in reversed(chunks) if c.get("usage")), None)
        self.record("streaming", s == 200 and "10" in text, total, usage,
                    f"chunks={len(chunks)} ttft={ttft:.2f}s usage_in_stream={usage is not None}", chunks[-3:])

    # --- tools
    def tools_single(self):
        s, r, h, dt = self.c.chat(messages=[{"role": "system", "content": SYSTEM},
                                            {"role": "user", "content": "Remind me at 6pm to call the bank."}],
                                  tools=TOOLS, reasoning_effort="none")
        calls = r["choices"][0]["message"].get("tool_calls") or [] if s == 200 else []
        ok = len(calls) == 1 and calls[0]["function"]["name"] == "reminders_create"
        args_ok = ok and not validate(json.loads(calls[0]["function"]["arguments"]),
                                      TOOLS[2]["function"]["parameters"])
        self.record("tools_single", ok and args_ok, dt, r.get("usage") if isinstance(r, dict) else None,
                    f"calls={[c['function']['name'] for c in calls]} args_valid={args_ok}", r)

    def tools_parallel(self):
        s, r, h, dt = self.c.chat(messages=[
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": "Did 45 minutes of the Rust course section 4 and 30 min gym. "
                                        "Also I need to renew my passport by Dec 20, high priority."}],
            tools=TOOLS, reasoning_effort="none")
        calls = r["choices"][0]["message"].get("tool_calls") or [] if s == 200 else []
        names = sorted(c["function"]["name"] for c in calls)
        bad = []
        for c in calls:
            schema = next(t["function"]["parameters"] for t in TOOLS if t["function"]["name"] == c["function"]["name"])
            bad += validate(json.loads(c["function"]["arguments"]), schema)
        ok = names.count("activities_log") == 2 and "tasks_create" in names and not bad
        self.record("tools_parallel", ok, dt, r.get("usage") if isinstance(r, dict) else None,
                    f"calls={names} schema_errors={bad}", r)
        self._parallel_msg = r["choices"][0]["message"] if s == 200 else None

    def tools_roundtrip(self):
        msg = getattr(self, "_parallel_msg", None)
        if not msg or not msg.get("tool_calls"):
            self.record("tools_roundtrip", False, notes="skipped: no tool calls from tools_parallel")
            return
        messages = [{"role": "system", "content": SYSTEM},
                    {"role": "user", "content": "Did 45 minutes of the Rust course section 4 and 30 min gym. "
                                                "Also I need to renew my passport by Dec 20, high priority."},
                    {"role": "assistant", "content": msg.get("content") or "", "tool_calls": msg["tool_calls"]}]
        for i, c in enumerate(msg["tool_calls"]):
            messages.append({"role": "tool", "tool_call_id": c.get("id") or f"call_{i}",
                             "content": json.dumps({"ok": True, "id": f"0193a0b4-{i:04d}"})})
        s, r, h, dt = self.c.chat(messages=messages, tools=TOOLS, reasoning_effort="none")
        out = r["choices"][0]["message"] if s == 200 else {}
        ok = s == 200 and bool(out.get("content")) and not out.get("tool_calls")
        self.record("tools_roundtrip", ok, dt, r.get("usage") if isinstance(r, dict) else None,
                    f"final={(out.get('content') or '')[:120]!r} ids_present={all('id' in c for c in msg['tool_calls'])}",
                    r)

    # --- structured output
    def structured_text(self):
        errors, lat = [], 0.0
        for i in range(3):
            s, r, h, dt = self.c.chat(
                messages=[{"role": "system", "content": "Classify the user's activity into segments."},
                          {"role": "user", "content": "Window: 'health.cc — TheLife_v2 — Visual Studio Code', "
                                                      "then 'Lecture 7: Eigenvalues — MIT 18.06 - Chromium'."}],
                response_format={"type": "json_schema",
                                 "json_schema": {"name": "detections", "strict": True, "schema": DETECTION_SCHEMA}},
                reasoning_effort="none")
            lat += dt
            if s != 200:
                errors.append(f"run{i}: status {s}")
                continue
            try:
                errors += [f"run{i}: {e}" for e in validate(json.loads(r["choices"][0]["message"]["content"]),
                                                            DETECTION_SCHEMA)]
            except json.JSONDecodeError:
                errors.append(f"run{i}: not JSON")
            (OUT / f"structured_text_{i}.json").write_text(json.dumps(r, indent=2, ensure_ascii=False))
        self.record("structured_text_x3", not errors, lat / 3, None, f"errors={errors}")

    # --- vision
    def vision_describe(self):
        s, r, h, dt = self.c.chat(messages=[{"role": "user", "content": [
            {"type": "text", "text": "What application and file is shown? Answer in one line."},
            {"type": "image_url", "image_url": {"url": synthetic_screen("ide")}}]}],
            reasoning_effort="none", max_tokens=300)
        content = (r["choices"][0]["message"].get("content") or "") if s == 200 else ""
        ok = s == 200 and "health" in content.lower()
        self.record("vision_describe", ok, dt, r.get("usage") if isinstance(r, dict) else None,
                    f"answer={content[:140]!r}", r)

    def vision_structured(self):
        errors, notes = [], []
        expected = {"ide": "work", "lecture": "skill", "game": "entertainment"}
        lat = 0.0
        for kind, want in expected.items():
            s, r, h, dt = self.c.chat(
                messages=[{"role": "system", "content":
                           "You judge what the user is doing from a screenshot plus context. "
                           "Areas: work (professional/software project), skill (learning), "
                           "entertainment (games, leisure video), fitness, personal, other."},
                          {"role": "user", "content": [
                              {"type": "text", "text": "Context: active process list includes code, chromium, "
                                                       "steam. Return the detection segments for this screen."},
                              {"type": "image_url", "image_url": {"url": synthetic_screen(kind)}}]}],
                response_format={"type": "json_schema",
                                 "json_schema": {"name": "detections", "strict": True, "schema": DETECTION_SCHEMA}},
                reasoning_effort="none")
            lat += dt
            (OUT / f"vision_structured_{kind}.json").write_text(json.dumps(r, indent=2, ensure_ascii=False))
            if s != 200:
                errors.append(f"{kind}: status {s}")
                continue
            try:
                obj = json.loads(r["choices"][0]["message"]["content"])
            except json.JSONDecodeError:
                errors.append(f"{kind}: not JSON")
                continue
            errors += [f"{kind}: {e}" for e in validate(obj, DETECTION_SCHEMA)]
            got = [seg.get("area") for seg in obj.get("segments", [])]
            notes.append(f"{kind}->{got}")
            if want not in got:
                errors.append(f"{kind}: expected area {want}, got {got}")
        self.record("vision_structured_x3", not errors, lat / 3, None, f"{notes} errors={errors}")

    # --- errors (fixtures for the classifier)
    def error_bodies(self):
        cases = {
            "unknown_model": dict(model_override="does-not-exist:cloud",
                                  messages=[{"role": "user", "content": "hi"}]),
            "bad_tool_schema": dict(messages=[{"role": "user", "content": "hi"}],
                                    tools=[{"type": "function", "function": {"name": "x", "parameters": "nope"}}]),
            "bad_role": dict(messages=[{"role": "robot", "content": "hi"}]),
        }
        fixtures, notes = {}, []
        for name, kw in cases.items():
            model = kw.pop("model_override", None)
            saved = self.c.model
            if model:
                self.c.model = model
            try:
                s, r, h, dt = self.c.chat(max_tokens=5, **kw)
            finally:
                self.c.model = saved
            fixtures[name] = {"status": s, "body": r, "headers": {k: v for k, v in h.items()
                                                                  if k.lower() in ("content-type", "retry-after")}}
            notes.append(f"{name}={s}")
        (OUT / "error_fixtures.json").write_text(json.dumps(fixtures, indent=2, ensure_ascii=False))
        self.record("error_bodies", True, notes=" ".join(notes) + " (see out/error_fixtures.json)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:11434/v1")
    ap.add_argument("--model", default="glm-5.3-flash:cloud")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)

    sp = Spike(Client(args.base_url, args.model))
    for name in ["chat_basic", "reasoning_off", "streaming", "tools_single", "tools_parallel", "tools_roundtrip",
                 "structured_text", "vision_describe", "vision_structured", "error_bodies"]:
        sp.run(name, getattr(sp, name))

    (OUT / "results.json").write_text(json.dumps({"model": args.model, "base_url": args.base_url,
                                                  "at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                                                  "results": sp.results}, indent=2))
    passed = sum(r["ok"] for r in sp.results)
    print(f"\n{passed}/{len(sp.results)} checks passed. Details in {OUT}/")


if __name__ == "__main__":
    main()
