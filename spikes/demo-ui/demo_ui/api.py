"""Thin HTTP client for the public API. No business logic: it sends what the views ask for and turns
RFC 9457 problem responses into ApiError."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any

import requests


@dataclass
class ApiError(Exception):
    status: int
    title: str
    detail: str = ""
    errors: list[dict] = field(default_factory=list)

    def __str__(self) -> str:
        return f"{self.status} {self.title}" + (f": {self.detail}" if self.detail else "")

    def field_errors(self) -> dict[str, str]:
        """Validation messages keyed by top-level field name ("/title" or "title" → "title")."""
        out: dict[str, str] = {}
        for e in self.errors:
            path = str(e.get("path", "")).lstrip("/").split("/")[0]
            out.setdefault(path, str(e.get("message", "")))
        return out


@dataclass
class Response:
    status: int
    body: Any
    etag: str | None


class Api:
    def __init__(self, base_url: str, token: str | None = None, *, session: requests.Session | None = None,
                 timeout: float = 15.0):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.http = session or requests.Session()
        self.timeout = timeout

    def call(self, method: str, path: str, *, params: dict | None = None, body: Any = None,
             if_match: int | str | None = None) -> Response:
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if method == "PATCH":
            headers["Content-Type"] = "application/merge-patch+json"  # updates are JSON Merge Patch (RFC 7396)
        if if_match is not None:
            headers["If-Match"] = f'"{if_match}"'
        query = {k: v for k, v in (params or {}).items() if v not in (None, "")}
        try:
            payload = None if body is None else json.dumps(body).encode()
            if payload is not None:
                headers.setdefault("Content-Type", "application/json")
            r = self.http.request(method, self.base_url + path, params=query, data=payload, headers=headers,
                                  timeout=self.timeout, allow_redirects=False)
        except requests.RequestException as e:
            raise ApiError(502, "Backend unreachable", type(e).__name__) from None
        data: Any = None
        if r.content:
            try:
                data = r.json()
            except ValueError:
                data = r.text
        if r.status_code >= 400:
            p = data if isinstance(data, dict) else {}
            raise ApiError(r.status_code, str(p.get("title") or r.reason or "Error"), str(p.get("detail") or ""),
                           list(p.get("errors") or []))
        return Response(r.status_code, data, r.headers.get("ETag"))

    def get(self, path: str, **params: Any) -> Any:
        return self.call("GET", path, params=params).body

    def post(self, path: str, body: Any = None) -> Any:
        return self.call("POST", path, body=body).body
