#!/usr/bin/env python3
"""A fake TheLife backend for the collector system tests (collector_lld.md §11).

Serves the device endpoints the collector uses under /api/v1 and records every request. Image parts of capture
uploads are never kept: only their size and first bytes are recorded. Tests steer it through /__control/*:

  GET  /__control/requests        recorded requests (JSON list)
  POST /__control/clear           forget recorded requests
  POST /__control/set             merge settings: config, etag, overrides, notifications, tokens, rotate_to,
                                  redirect_config
  GET  /__control/state           acked notifications, stored client_seqs

Usage: fake_backend.py PORT   (0 picks a free port; prints "READY <port>" once listening)
"""
import json
import sys
import threading
import time
import uuid
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

LOCK = threading.Lock()
STATE = {
    "requests": [],
    "config": {
        "enabled": True, "capture_interval_s": 300, "jitter_pct": 10, "capture_monitors": "all",
        "capture_max_px": 1600, "idle_threshold_s": 120, "process_interval_s": 15,
    },
    "config_raw": None,      # a literal body to serve instead of `config` (hostile-backend tests)
    "etag": '"c1"',
    "overrides": {},         # "METHOD /path-prefix" -> {"status": n, "body": str, "headers": {}}
    "notifications": [],
    "acked": [],
    "seqs": [],
    "tokens": [],            # accepted bearer tokens; empty accepts any well-formed one
    "rotate_to": None,
    "redirect_config": False,
}


def parse_multipart(content_type, body):
    msg = BytesParser(policy=HTTP).parsebytes(
        b"Content-Type: " + content_type.encode() + b"\r\nMIME-Version: 1.0\r\n\r\n" + body)
    parts = []
    for part in msg.iter_parts():
        payload = part.get_payload(decode=True) or b""
        entry = {
            "name": part.get_param("name", header="content-disposition"),
            "filename": part.get_filename(),
            "content_type": part.get_content_type(),
            "size": len(payload),
            "magic": payload[:3].hex(),
        }
        if not entry["filename"]:
            entry["text"] = payload.decode("utf-8", "replace")
        parts.append(entry)
    return parts


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=None, headers=None):
        data = b"" if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def control(self, method, path):
        with LOCK:
            if method == "GET" and path == "/__control/requests":
                return self.reply(200, STATE["requests"])
            if method == "GET" and path == "/__control/state":
                return self.reply(200, {"acked": STATE["acked"], "seqs": STATE["seqs"]})
            if method == "POST" and path == "/__control/clear":
                STATE["requests"] = []
                return self.reply(204)
            if method == "POST" and path == "/__control/set":
                STATE.update(json.loads(self.body() or b"{}"))
                return self.reply(204)
        self.reply(404, {"title": "no such control"})

    def handle_any(self, method):
        url = urlsplit(self.path)
        path = url.path
        if path.startswith("/__control/"):
            return self.control(method, path)
        raw = self.body() if method == "POST" else b""
        auth = self.headers.get("Authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else ""
        rec = {"method": method, "path": path, "query": url.query, "token": token,
               "headers": {k.lower(): v for k, v in self.headers.items() if k.lower() != "authorization"},
               "at": time.time()}
        ctype = self.headers.get("Content-Type", "")
        if ctype.startswith("multipart/form-data"):
            rec["parts"] = parse_multipart(ctype, raw)
        elif raw:
            try:
                rec["json"] = json.loads(raw)
            except ValueError:
                rec["raw"] = raw.decode("utf-8", "replace")
        with LOCK:
            STATE["requests"].append(rec)
            tokens = list(STATE["tokens"])
            override = None
            for key, o in STATE["overrides"].items():
                m, prefix = key.split(" ", 1)
                if m == method and path.startswith(prefix):
                    override = o
        if tokens and token not in tokens:
            return self.reply(401, {"title": "Unauthorized"})
        if override:
            body = override.get("body")
            return self.reply(override["status"], body.encode() if isinstance(body, str) else body,
                              override.get("headers"))

        if method == "GET" and path == "/api/v1/devices/self/config":
            with LOCK:
                if STATE["redirect_config"]:
                    return self.reply(302, None, {"Location": "/api/v1/evil"})
                etag = STATE["etag"]
                if self.headers.get("If-None-Match") == etag:
                    return self.reply(304, None, {"ETag": etag})
                body = STATE["config_raw"].encode() if STATE["config_raw"] else STATE["config"]
            return self.reply(200, body, {"ETag": etag})
        if method == "POST" and path == "/api/v1/ingest/observations":
            if not self.headers.get("Idempotency-Key"):
                return self.reply(422, {"errors": [{"path": "Idempotency-Key", "message": "is required"}]})
            accepted, dup = [], []
            with LOCK:
                for o in rec.get("json", {}).get("observations", []):
                    s = o["client_seq"]
                    if s in STATE["seqs"]:
                        dup.append(s)
                    else:
                        STATE["seqs"].append(s)
                        accepted.append(s)
            return self.reply(200, {"accepted": accepted, "duplicates": dup})
        if method == "POST" and path == "/api/v1/ingest/captures":
            return self.reply(202, {"capture_id": str(uuid.uuid4())})
        if method == "POST" and path == "/api/v1/devices/self/heartbeat":
            return self.reply(204)
        if method == "POST" and path == "/api/v1/devices/self/token:rotate":
            with LOCK:
                new = STATE["rotate_to"]
                if not new:
                    return self.reply(500, {"title": "no rotation configured"})
                STATE["tokens"] = [token, new]
            return self.reply(200, {"token": new, "previous_expires_at": "2030-01-01T00:00:00Z"})
        if method == "GET" and path == "/api/v1/devices/self/notifications":
            wait = min(int(parse_qs(url.query).get("wait", ["0"])[0]), 2)  # short, to keep the tests fast
            deadline = time.time() + wait
            while True:
                with LOCK:
                    items = [n for n in STATE["notifications"] if n["id"] not in STATE["acked"]]
                if items or time.time() >= deadline:
                    return self.reply(200, {"items": items})
                time.sleep(0.1)
        if method == "POST" and path.startswith("/api/v1/devices/self/notifications/") and path.endswith(":ack"):
            nid = path[len("/api/v1/devices/self/notifications/"):-len(":ack")]
            with LOCK:
                STATE["acked"].append(nid)
            return self.reply(204)
        return self.reply(404, {"title": "Not Found"})

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    ThreadingHTTPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print(f"READY {srv.server_address[1]}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
