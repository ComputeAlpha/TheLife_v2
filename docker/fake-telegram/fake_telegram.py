#!/usr/bin/env python3
"""Fake Telegram Bot API for tests (agent_lld.md §7.2). Standard library only.

Bot methods under /bot<token>/<method> (a wrong token answers 401 like Telegram):
  getUpdates {offset, timeout}   long polls the queued updates with update_id >= offset (timeout capped at 2 s)
  sendMessage, sendChatAction, answerCallbackQuery, editMessageText
                                 recorded; sendMessage returns {"message_id": n}
Control endpoints:
  POST /_fake/update {...}       queues an update (update_id assigned)
  GET  /_fake/calls              [{method, body}] of the bot calls received, oldest first (getUpdates excluded)
  POST /_fake/fail {"method": "sendMessage", "count": 1}   the next N calls of that method answer 500
  POST /_fake/reset
  GET  /_fake/health

  python3 fake_telegram.py --port 0 --token T     prints "fake-telegram listening on <port>" once ready
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

cond = threading.Condition()
updates = []
calls = []
fail = {}
counter = {"update": 0, "message": 1000}
TOKEN = ""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def send_json(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        return json.loads(raw) if raw else {}

    def do_GET(self):
        if self.path == "/_fake/health":
            return self.send_json(200, {"ok": True})
        if self.path == "/_fake/calls":
            with cond:
                return self.send_json(200, list(calls))
        self.send_json(404, {"ok": False})

    def do_POST(self):
        body = self.read_body()
        if self.path == "/_fake/update":
            with cond:
                counter["update"] += 1
                body["update_id"] = counter["update"]
                updates.append(body)
                cond.notify_all()
            return self.send_json(200, {"update_id": body["update_id"]})
        if self.path == "/_fake/fail":
            with cond:
                fail[body["method"]] = int(body.get("count", 1))
            return self.send_json(200, {})
        if self.path == "/_fake/reset":
            with cond:
                updates.clear()
                calls.clear()
                fail.clear()
            return self.send_json(200, {})
        parts = self.path.split("/")
        if len(parts) != 3 or not parts[1].startswith("bot"):
            return self.send_json(404, {"ok": False, "error_code": 404, "description": "Not Found"})
        if parts[1][3:] != TOKEN:
            return self.send_json(401, {"ok": False, "error_code": 401, "description": "Unauthorized"})
        method = parts[2]
        if method == "getUpdates":
            offset = int(body.get("offset", 0))
            deadline = time.time() + min(float(body.get("timeout", 0)), 2.0)
            with cond:
                while True:
                    # Telegram forgets updates below the offset once it is acknowledged.
                    updates[:] = [u for u in updates if u["update_id"] >= offset]
                    if updates or time.time() >= deadline:
                        return self.send_json(200, {"ok": True, "result": list(updates)})
                    cond.wait(max(0.0, deadline - time.time()))
        with cond:
            calls.append({"method": method, "body": body})
            if fail.get(method, 0) > 0:
                fail[method] -= 1
                return self.send_json(500, {"ok": False, "error_code": 500, "description": "Internal"})
            counter["message"] += 1
            mid = counter["message"]
        if method in ("sendMessage", "editMessageText"):
            return self.send_json(200, {"ok": True, "result": {"message_id": mid, "chat": {"id": body.get("chat_id")},
                                                               "text": body.get("text", "")}})
        return self.send_json(200, {"ok": True, "result": True})


def main():
    global TOKEN
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--token", required=True)
    args = ap.parse_args()
    TOKEN = args.token
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    print(f"fake-telegram listening on {srv.server_address[1]}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
