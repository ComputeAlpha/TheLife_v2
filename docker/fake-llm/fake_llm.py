#!/usr/bin/env python3
"""Fake OpenAI-compatible LLM server for tests (agent_lld.md §3.7). Standard library only.

Responses come from a FIFO script. Each entry is one of:
  {"content": "...", "tool_calls": [{"name": "x", "arguments": {...} | "json text", "id": "c1"}],
   "reasoning": "...", "finish_reason": "stop", "usage": {"prompt_tokens": 1, "completion_tokens": 1}}
  {"fixture": "tools_single"}              a recorded Stage 0 response body (fixtures/<name>.json)
  {"error": {"status": 429, "body": {...} | "text", "headers": {"Retry-After": "2"}}}
  {"error_fixture": "unknown_model"}       a recorded error (fixtures/error_fixtures.json)
  {"drop": true}                           close the connection without answering (transport error)
Any entry may add "delay_ms" (before answering) and, when the request streams, "break_after": N (send N
chunks, then close without finishing). With an empty script the reply is "echo: <last user message>".

Web search (Ollama web search API shape): POST /api/web_search {query, max_results} and POST /api/web_fetch
{url} answer from their own FIFO (POST /_fake/web_script {"responses": [...]}); each entry is a response body, or
{"error": {"status": 401, "body": ...}}. With an empty queue they answer with one canned result echoing the input.
GET /_fake/web_requests lists {path, body, authorization} of the web calls received.

Control endpoints:
  POST /_fake/script {"responses": [...], "append": false}
  POST /_fake/reset
  GET  /_fake/requests        request bodies received so far (JSON array)
  GET  /_fake/health

  python3 fake_llm.py --port 0     prints "fake-llm listening on <port>" once ready
"""
import argparse
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")

lock = threading.Lock()
script = []
requests_seen = []
web_script = []
web_seen = []
counter = [0]


def load_fixture(name):
    if not name.replace("_", "").isalnum():
        raise ValueError("bad fixture name")
    with open(os.path.join(FIXTURES, name + ".json"), encoding="utf-8") as f:
        return json.load(f)


def next_id(prefix):
    with lock:
        counter[0] += 1
        return f"{prefix}{counter[0]}"


def tool_calls_of(entry):
    out = []
    for i, c in enumerate(entry.get("tool_calls") or []):
        args = c.get("arguments", {})
        out.append({
            "id": c.get("id") or next_id("call_"),
            "index": i,
            "type": "function",
            "function": {"name": c["name"], "arguments": args if isinstance(args, str) else json.dumps(args)},
        })
    return out


def completion_body(entry, model):
    if "fixture" in entry:
        return load_fixture(entry["fixture"])
    calls = tool_calls_of(entry)
    msg = {"role": "assistant", "content": entry.get("content", "")}
    if entry.get("reasoning"):
        msg["reasoning"] = entry["reasoning"]
    if calls:
        msg["tool_calls"] = calls
    finish = entry.get("finish_reason") or ("tool_calls" if calls else "stop")
    usage = entry.get("usage") or {"prompt_tokens": 10, "completion_tokens": 5}
    usage = dict(usage, total_tokens=usage.get("prompt_tokens", 0) + usage.get("completion_tokens", 0))
    return {
        "id": next_id("chatcmpl-"), "object": "chat.completion", "created": int(time.time()), "model": model,
        "choices": [{"index": 0, "message": msg, "finish_reason": finish}], "usage": usage,
    }


def stream_chunks(body):
    """Turns a completion body into chat.completion.chunk events (content split by words)."""
    choice = body["choices"][0]
    msg = choice.get("message", {})
    base = {"id": body.get("id", "chatcmpl-x"), "object": "chat.completion.chunk",
            "created": body.get("created", 0), "model": body.get("model", "fake")}
    chunks = []
    if msg.get("reasoning"):
        chunks.append(dict(base, choices=[{"index": 0, "delta": {"reasoning": msg["reasoning"]}, "finish_reason": None}]))
    content = msg.get("content") or ""
    words = content.split(" ")
    for i, w in enumerate(words):
        piece = w if i == len(words) - 1 else w + " "
        if piece:
            chunks.append(dict(base, choices=[{"index": 0, "delta": {"content": piece}, "finish_reason": None}]))
    for c in msg.get("tool_calls") or []:
        args = c["function"]["arguments"]
        half = len(args) // 2
        chunks.append(dict(base, choices=[{"index": 0, "delta": {"tool_calls": [
            {"index": c.get("index", 0), "id": c["id"], "type": "function",
             "function": {"name": c["function"]["name"], "arguments": args[:half]}}]}, "finish_reason": None}]))
        chunks.append(dict(base, choices=[{"index": 0, "delta": {"tool_calls": [
            {"index": c.get("index", 0), "function": {"arguments": args[half:]}}]}, "finish_reason": None}]))
    chunks.append(dict(base, choices=[{"index": 0, "delta": {}, "finish_reason": choice.get("finish_reason")}]))
    chunks.append(dict(base, choices=[], usage=body.get("usage", {})))
    return chunks


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def send_json(self, status, obj, headers=None, content_type="application/json"):
        data = (obj if isinstance(obj, str) else json.dumps(obj)).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        return json.loads(raw) if raw else {}

    def do_GET(self):
        if self.path == "/_fake/health":
            return self.send_json(200, {"status": "ok"})
        if self.path == "/_fake/requests":
            with lock:
                return self.send_json(200, list(requests_seen))
        if self.path == "/_fake/web_requests":
            with lock:
                return self.send_json(200, list(web_seen))
        if self.path == "/v1/models":
            return self.send_json(200, {"object": "list", "data": [{"id": "fake", "object": "model"}]})
        self.send_json(404, {"error": {"message": "not found", "type": "not_found_error"}})

    def do_POST(self):
        try:
            body = self.read_body()
        except ValueError:
            return self.send_json(400, {"error": {"message": "bad json", "type": "invalid_request_error"}})
        if self.path == "/_fake/script":
            with lock:
                if not body.get("append"):
                    script.clear()
                script.extend(body.get("responses", []))
            return self.send_json(200, {"queued": len(script)})
        if self.path == "/_fake/reset":
            with lock:
                script.clear()
                requests_seen.clear()
                web_script.clear()
                web_seen.clear()
            return self.send_json(200, {"status": "reset"})
        if self.path == "/_fake/web_script":
            with lock:
                web_script.clear()
                web_script.extend(body.get("responses", []))
            return self.send_json(200, {"queued": len(web_script)})
        if self.path in ("/api/web_search", "/api/web_fetch"):
            with lock:
                web_seen.append({"path": self.path, "body": body, "authorization": self.headers.get("Authorization")})
                entry = web_script.pop(0) if web_script else None
            if entry is None:
                if self.path == "/api/web_search":
                    entry = {"results": [{"title": "Result for " + str(body.get("query")), "url": "https://example.org/r1",
                                          "content": "Canned search result."}]}
                else:
                    entry = {"title": "Page", "content": "Canned page text for " + str(body.get("url")), "links": []}
            if "error" in entry:
                e = entry["error"]
                return self.send_json(e.get("status", 500), e.get("body", ""))
            return self.send_json(200, entry)
        if self.path != "/v1/chat/completions":
            return self.send_json(404, {"error": {"message": "not found", "type": "not_found_error"}})

        with lock:
            requests_seen.append(body)
            entry = script.pop(0) if script else None
        if entry is None:
            last = next((m.get("content") for m in reversed(body.get("messages", [])) if m.get("role") == "user"), "")
            if isinstance(last, list):
                last = " ".join(p.get("text", "") for p in last if isinstance(p, dict))
            entry = {"content": f"echo: {last}"}
        if entry.get("delay_ms"):
            time.sleep(entry["delay_ms"] / 1000.0)
        if entry.get("drop"):
            self.close_connection = True
            self.connection.shutdown(2)
            return
        if "error_fixture" in entry:
            fx = load_fixture("error_fixtures")[entry["error_fixture"]]
            entry = {"error": fx}
        if "error" in entry:
            e = entry["error"]
            headers = dict(e.get("headers") or {})
            ctype = headers.pop("Content-Type", "application/json")
            return self.send_json(e.get("status", 500), e.get("body", ""), headers, ctype)

        out = completion_body(entry, body.get("model", "fake"))
        if not body.get("stream"):
            return self.send_json(200, out)

        chunks = stream_chunks(out)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        def write_chunk(data):
            self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
            self.wfile.flush()

        limit = entry.get("break_after")
        for i, c in enumerate(chunks):
            if limit is not None and i >= limit:
                self.close_connection = True
                self.connection.shutdown(2)
                return
            write_chunk(f"data: {json.dumps(c)}\n\n".encode())
        write_chunk(b"data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=11435)
    args = ap.parse_args()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    print(f"fake-llm listening on {srv.server_address[1]}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
