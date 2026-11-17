"""Server-side login sessions. The browser holds only a random session id (HttpOnly, SameSite=Strict); the
API session token stays in this process and is revoked at logout. Restarting the UI logs everyone out."""
from __future__ import annotations

import secrets
import threading
import time
from dataclasses import dataclass, field


@dataclass
class Session:
    token: str
    user: dict
    csrf: str = field(default_factory=lambda: secrets.token_urlsafe(32))
    seen: float = field(default_factory=time.monotonic)


class SessionStore:
    def __init__(self, idle_seconds: int = 12 * 3600):
        self.idle = idle_seconds
        self._items: dict[str, Session] = {}
        self._lock = threading.Lock()

    def create(self, token: str, user: dict) -> str:
        sid = secrets.token_urlsafe(32)
        with self._lock:
            self._items[sid] = Session(token, user)
        return sid

    def get(self, sid: str | None) -> Session | None:
        if not sid:
            return None
        now = time.monotonic()
        with self._lock:
            for k in [k for k, s in self._items.items() if now - s.seen > self.idle]:
                del self._items[k]
            s = self._items.get(sid)
            if s:
                s.seen = now
            return s

    def drop(self, sid: str | None) -> Session | None:
        with self._lock:
            return self._items.pop(sid, None) if sid else None

    def __len__(self) -> int:
        return len(self._items)
