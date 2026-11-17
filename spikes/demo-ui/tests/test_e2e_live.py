"""End to end through the real backend. Opt-in: set THELIFE_DEMO_E2E_API (backend base URL) and
THELIFE_DEMO_E2E_EMAIL / THELIFE_DEMO_E2E_PASSWORD for an account made with thelife-admin. Use a throwaway
tenant: the test creates and deletes data."""
import os
import re
from datetime import datetime, timedelta

import pytest

from demo_ui import create_app

API = os.environ.get("THELIFE_DEMO_E2E_API")
pytestmark = pytest.mark.skipif(not API, reason="THELIFE_DEMO_E2E_API not set")


def csrf_of(html):
    return re.search(r'name="csrf" value="([^"]+)"', html).group(1)


def created_id(resp):
    return re.search(r"/r/[a-z_]+/([0-9a-f-]{36})$", resp.headers["Location"]).group(1)


@pytest.fixture
def ui():
    app = create_app({"API_URL": API, "TESTING": True})
    c = app.test_client()
    page = c.get("/login").get_data(as_text=True)
    r = c.post("/login", data={"csrf": csrf_of(page), "email": os.environ["THELIFE_DEMO_E2E_EMAIL"],
                               "password": os.environ["THELIFE_DEMO_E2E_PASSWORD"]})
    assert r.status_code == 302, r.get_data(as_text=True)
    c.csrf = csrf_of(c.get("/settings").get_data(as_text=True))
    return c


def test_task_lifecycle_and_calendar(ui):
    r = ui.post("/r/tasks/new", data={"csrf": ui.csrf, "title": "e2e stretch", "area": "fitness", "priority": "1",
                                      "status": "open", "rrule": "FREQ=DAILY"})
    assert r.status_code == 302, r.get_data(as_text=True)
    tid = created_id(r)
    page = ui.get(f"/r/tasks/{tid}").get_data(as_text=True)
    assert "e2e stretch" in page and "Occurrences" in page

    cal = ui.get("/").get_data(as_text=True)
    m = re.search(rf"/tasks/{tid}/occurrences/(\d{{4}}-\d\d-\d\d)/complete", cal)
    assert m, "today's occurrence is on the calendar"
    r = ui.post(m.group(0), data={"csrf": ui.csrf, "next": "/"})
    assert r.status_code == 302
    assert "done." in ui.get("/").get_data(as_text=True)  # flash
    occ = ui.get(f"/r/tasks/{tid}").get_data(as_text=True)
    assert re.search(rf"<td>{m.group(1)}</td><td>done", occ)

    version = re.search(r'name="version" value="(\d+)"', occ).group(1)
    r = ui.post(f"/r/tasks/{tid}/edit", data={"csrf": ui.csrf, "version": version, "title": "e2e stretch more",
                                              "area": "fitness", "priority": "2", "status": "open",
                                              "rrule": "FREQ=DAILY"})
    assert r.status_code == 302
    page = ui.get(f"/r/tasks/{tid}").get_data(as_text=True)
    assert "e2e stretch more" in page
    stale = ui.post(f"/r/tasks/{tid}/edit", data={"csrf": ui.csrf, "version": version, "title": "lost update",
                                                  "area": "fitness"})
    assert stale.status_code == 302 and "lost update" not in ui.get(f"/r/tasks/{tid}").get_data(as_text=True)

    version = re.search(r'name="version" value="(\d+)"', ui.get(f"/r/tasks/{tid}").get_data(as_text=True)).group(1)
    assert ui.post(f"/r/tasks/{tid}/delete", data={"csrf": ui.csrf, "version": version}).status_code == 302


def test_invalid_rrule_shows_server_message(ui):
    r = ui.post("/r/tasks/new", data={"csrf": ui.csrf, "title": "never", "area": "work",
                                      "rrule": "FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30"})
    assert r.status_code == 422 and "can never occur" in r.get_data(as_text=True)


def test_detection_dispute_and_history(ui):
    start = datetime.now().replace(microsecond=0) - timedelta(hours=2)
    r = ui.post("/r/detections/new", data={"csrf": ui.csrf, "source": "user", "area": "skill", "category": "reading",
                                           "summary": "e2e read a book", "confidence": "1",
                                           "started_at": start.isoformat(),
                                           "ended_at": (start + timedelta(hours=1)).isoformat()})
    assert r.status_code == 302, r.get_data(as_text=True)
    did = created_id(r)
    # a user's own detection starts confirmed; confirming again is a conflict shown as a message
    page = ui.post(f"/r/detections/{did}/do/confirm", data={"csrf": ui.csrf}, follow_redirects=True)
    assert "already confirmed" in page.get_data(as_text=True)
    r = ui.post(f"/r/detections/{did}/do/dispute", data={"csrf": ui.csrf, "reason": "was a podcast"})
    assert r.status_code == 302
    page = ui.get(f"/r/detections/{did}").get_data(as_text=True)
    assert re.search(r"<td>\s*confirmed\s*</td><td>disputed</td><td>\s*was a podcast\s*</td>", page)


def test_device_token_is_shown_once(ui):
    r = ui.post("/r/devices/new", data={"csrf": ui.csrf, "name": "e2e laptop", "platform": "linux-x11"})
    html = r.get_data(as_text=True)
    token = re.search(r"tld_[A-Za-z0-9_-]+", html)
    assert r.status_code == 200 and token and r.headers["Cache-Control"] == "no-store"
    did = re.search(r"/r/devices/([0-9a-f-]{36})", html).group(1)
    assert token.group(0) not in ui.get(f"/r/devices/{did}").get_data(as_text=True)
    assert ui.post(f"/r/devices/{did}/delete", data={"csrf": ui.csrf}).status_code == 302
    assert "revoked_at" in ui.get(f"/r/devices/{did}").get_data(as_text=True)


def test_every_resource_page_renders(ui):
    for prefix in ui.application.extensions["demo"]["contract"].resources:
        assert ui.get(f"/r/{prefix}").status_code == 200, prefix


def test_chat_command_round_trip(ui):
    """Deterministic commands answer without a model, so this works whether or not the backend has an LLM."""
    page = ui.get("/chat").get_data(as_text=True)
    client_id = re.search(r'name="client_id" value="([0-9a-f]+)"', page).group(1)
    r = ui.post("/chat", data={"csrf": ui.csrf, "text": "/help", "client_id": client_id})
    assert r.status_code == 302 and "/chat?turn=" in r.headers["Location"]
    turn_url = r.headers["Location"]
    for _ in range(100):
        html = ui.get(turn_url).get_data(as_text=True)
        if "working on it" not in html:
            break
    assert "/approve N, /reject N" in html
    # Resubmitting the same form does not send the message twice.
    again = ui.post("/chat", data={"csrf": ui.csrf, "text": "/help", "client_id": client_id})
    assert again.headers["Location"] == turn_url


def test_timeline_correct_and_dispute(ui):
    """Stage 4: a detection shows on the timeline; correcting it supersedes it, disputing stops it counting."""
    start = datetime.now().replace(second=0, microsecond=0) - timedelta(hours=3)
    r = ui.post("/r/detections/new", data={"csrf": ui.csrf, "source": "user", "area": "entertainment",
                                           "category": "video", "summary": "e2e timeline guess", "confidence": "1",
                                           "started_at": start.isoformat(),
                                           "ended_at": (start + timedelta(minutes=40)).isoformat()})
    assert r.status_code == 302, r.get_data(as_text=True)
    did = created_id(r)

    def page_with(text):
        for day in (None, (start - timedelta(days=1)).date().isoformat(), start.date().isoformat()):
            url = "/timeline" + (f"?day={day}" if day else "")
            html = ui.get(url).get_data(as_text=True)
            if text in html:
                return url, html
        raise AssertionError(f"{text!r} not on the timeline")

    url, html = page_with("e2e timeline guess")
    assert f'id="d-{did}"' in html
    day = re.search(rf'/timeline/{did}/claim".*?name="day" value="([0-9-]+)"', html, re.S).group(1)
    r = ui.post(f"/timeline/{did}/claim", data={"csrf": ui.csrf, "next": url, "day": day,
                                               "start": start.strftime("%H:%M"),
                                               "end": (start + timedelta(minutes=30)).strftime("%H:%M"),
                                               "area": "skill", "category": "lecture",
                                               "summary": "e2e timeline lecture"})
    assert r.status_code == 302
    html = ui.get(url).get_data(as_text=True)
    assert "Corrected" in html and "e2e timeline lecture" in html
    assert f'id="d-{did}"' not in html  # superseded: hidden unless asked for
    assert f'id="d-{did}"' in ui.get(url + ("&" if "?" in url else "?") + "show=all").get_data(as_text=True)

    new_id = re.search(r'id="d-([0-9a-f-]{36})"[^>]*>\s*<span class="when">[^<]*</span>\s*<a[^>]*>e2e timeline lecture',
                       html).group(1)
    r = ui.post(f"/timeline/{new_id}/dispute", data={"csrf": ui.csrf, "next": url, "reason": "e2e it was a podcast"})
    assert r.status_code == 302
    html = ui.get(url).get_data(as_text=True)
    assert re.search(rf'class="entry detection source-user status-disputed" id="d-{new_id}"', html)
    history = ui.get(f"/r/detections/{new_id}").get_data(as_text=True)
    assert "e2e it was a podcast" in history
