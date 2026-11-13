#!/usr/bin/env python3
"""Fake thelife-embedder for tests (same HTTP-over-Unix-socket API, standard library only).

Vectors are deterministic: the normalised sum of one pseudo-random unit vector per word (lower-cased), after a
small synonym table, so texts sharing words, or synonyms, are similar and unrelated texts are not. Enough to tell
semantic retrieval from keyword search in tests; not a language model.

  python3 -I fake_embedder.py --socket /tmp/x/embedder.sock     prints "fake-embedder ready" when listening
"""
import argparse
import hashlib
import http.server
import json
import math
import os
import random
import re
import socketserver
import sys

DIM = 1024
SYNONYMS = {"workout": "gym", "exercise": "gym", "exercised": "gym", "fitness": "gym", "lifting": "gym",
            "physician": "doctor", "dentist": "doctor", "appointment": "doctor"}
_cache = {}


def word_vec(w):
    if w not in _cache:
        rnd = random.Random(int(hashlib.md5(w.encode()).hexdigest(), 16))
        v = [rnd.gauss(0, 1) for _ in range(DIM)]
        n = math.sqrt(sum(x * x for x in v))
        _cache[w] = [x / n for x in v]
    return _cache[w]


def embed(text):
    words = [SYNONYMS.get(w, w) for w in re.findall(r"\w+", text.lower())] or ["<empty>"]
    acc = [0.0] * DIM
    for w in words:
        for i, x in enumerate(word_vec(w)):
            acc[i] += x
    n = math.sqrt(sum(x * x for x in acc)) or 1.0
    return [round(x / n, 6) for x in acc]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, obj):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/v1/health":
            return self.reply(200, {"status": "ok", "provider": "fake", "model": "bge-m3", "model_version": "fake-1",
                                    "dim": DIM})
        self.reply(404, {"title": "Not Found"})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        if self.path != "/v1/embed":
            return self.reply(404, {"title": "Not Found"})
        texts = body.get("texts") or []
        if not 1 <= len(texts) <= 8:
            return self.reply(400, {"title": "Bad Request", "detail": "1 to 8 texts per call"})
        self.reply(200, {"model": "bge-m3", "model_version": "fake-1", "dim": DIM, "vectors": [embed(t) for t in texts]})


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    args = ap.parse_args()
    if os.path.exists(args.socket):
        os.unlink(args.socket)
    srv = Server(args.socket, Handler)
    print("fake-embedder ready", flush=True)
    try:
        srv.serve_forever()
    finally:
        os.unlink(args.socket)


if __name__ == "__main__":
    sys.exit(main())
