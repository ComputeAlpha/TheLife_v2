"""Fixtures: the Flask app wired to a fake backend through a requests transport adapter."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import pytest
import requests
from requests.adapters import BaseAdapter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from demo_ui import create_app  # noqa: E402

USER = {"id": "0190a000-0000-7000-8000-000000000001", "email": "me@example.test", "display_name": "Me",
        "timezone": "Asia/Kolkata", "day_start_hour": 4, "role": "user", "workdays": ["MO", "TU"]}


class FakeBackend(BaseAdapter):
    """Routes (method, path regex) to handlers; records every request it sees."""

    def __init__(self):
        super().__init__()
        self.routes: list[tuple[str, re.Pattern, object]] = []
        self.calls: list[dict] = []
        self.on("POST", r"/api/v1/auth/login", self.login)
        self.on("POST", r"/api/v1/auth/logout", lambda req: (204, None))
        self.on("GET", r"/api/v1/me", lambda req: (200, USER))

    def on(self, method, pattern, handler):
        self.routes.insert(0, (method, re.compile(pattern + r"$"), handler))

    @staticmethod
    def login(req):
        if req["body"] == {"email": "me@example.test", "password": "correct horse"}:
            return 200, {"token": "tlu_secret", "expires_at": "2026-12-12T00:00:00+00:00", "user": USER}
        return 401, {"type": "about:blank", "title": "Unauthorized", "status": 401}

    def last(self, method, path_re):
        return next(c for c in reversed(self.calls) if c["method"] == method and re.fullmatch(path_re, c["path"]))

    def send(self, request, **kwargs):
        parts = urlsplit(request.url)
        req = {"method": request.method, "path": parts.path, "query": {k: v[0] for k, v in parse_qs(parts.query).items()},
               "headers": dict(request.headers),
               "body": json.loads(request.body) if request.body else None}
        self.calls.append(req)
        status, body = 404, {"title": "Not Found", "status": 404}
        for method, pattern, handler in self.routes:
            if method == request.method and pattern.fullmatch(parts.path):
                status, body = handler(req)
                break
        r = requests.Response()
        r.status_code = status
        r.url = request.url
        r.request = request
        r.headers["Content-Type"] = "application/problem+json" if status >= 400 else "application/json"
        r._content = b"" if body is None else json.dumps(body).encode()
        return r

    def close(self):
        pass


@pytest.fixture
def backend():
    return FakeBackend()


@pytest.fixture
def app(backend):
    http = requests.Session()
    http.mount("http://fake", backend)
    return create_app({"API_URL": "http://fake", "HTTP_SESSION": http, "TESTING": True})


@pytest.fixture
def client(app):
    return app.test_client()


def csrf_of(html: str) -> str:
    return re.search(r'name="csrf" value="([^"]+)"', html).group(1)


@pytest.fixture
def signed_in(client):
    page = client.get("/login").get_data(as_text=True)
    r = client.post("/login", data={"email": "me@example.test", "password": "correct horse", "csrf": csrf_of(page)})
    assert r.status_code == 302
    return client


def session_csrf(client) -> str:
    return csrf_of(client.get("/settings").get_data(as_text=True))
