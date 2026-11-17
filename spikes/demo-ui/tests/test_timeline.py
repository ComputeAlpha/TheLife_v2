"""The detection timeline (Stage 4): one local day of detections, confirm / dispute / correct, collector status."""
from datetime import datetime, timedelta, timezone

from conftest import csrf_of, session_csrf

D1 = "0190a000-0000-7000-8000-0000000000d1"
D2 = "0190a000-0000-7000-8000-0000000000d2"
D3 = "0190a000-0000-7000-8000-0000000000d3"
D4 = "0190a000-0000-7000-8000-0000000000d4"


def det(id, start, end, **kw):
    return {"id": id, "source": "process", "started_at": start, "ended_at": end, "area": "work", "category": "coding",
            "summary": "VS Code: TheLife_v2", "confidence": 0.7, "status": "detected", "evidence": {"app": "VS Code"},
            "device_id": None, "supersedes_id": None, "superseded_by_id": None, **kw}


ITEMS = [
    # out of order on purpose; times in UTC (IST = UTC+5:30)
    det(D2, "2026-11-16T05:00:00+00:00", "2026-11-16T05:30:00+00:00", source="vision",
        summary="Writing ingest tests", confidence=0.9),
    det(D1, "2026-11-16T04:30:00+00:00", "2026-11-16T05:15:00+00:00"),
    det(D3, "2026-11-16T06:00:00+00:00", "2026-11-16T07:00:00+00:00", area="entertainment", category="video",
        summary="YouTube", status="disputed"),
    det(D4, "2026-11-16T08:00:00+00:00", "2026-11-16T08:20:00+00:00", status="superseded", summary="Old guess"),
]


def setup(backend, items=ITEMS, devices=()):
    backend.on("GET", r"/api/v1/detections", lambda req: (200, {"items": items, "next_cursor": None}))
    backend.on("GET", r"/api/v1/devices", lambda req: (200, {"items": list(devices), "next_cursor": None}))


def test_timeline_shows_the_local_day_in_order(signed_in, backend):
    setup(backend)
    html = signed_in.get("/timeline?day=2026-11-16").get_data(as_text=True)
    q = backend.last("GET", "/api/v1/detections")["query"]
    assert q["from"] == "2026-11-16T04:00:00+05:30" and q["to"] == "2026-11-17T04:00:00+05:30"  # day_start_hour 4
    assert html.index("10:00–10:45") < html.index("10:30–11:00")  # sorted by start, shown in IST
    assert "VS Code: TheLife_v2" in html and "Writing ingest tests" in html
    assert "Old guess" not in html and "1 corrected detection hidden" in html
    # 10:00-11:00 of work (two overlapping detections counted once); the disputed hour does not count.
    assert "1h 00m" in html and "entertainment</span> " not in html
    html = signed_in.get("/timeline?day=2026-11-16&show=all").get_data(as_text=True)
    assert "Old guess" in html


def test_review_actions_post_the_contract_bodies(signed_in, backend):
    setup(backend)
    backend.on("POST", r"/api/v1/detections/[0-9a-f-]+:(confirm|dispute|claim)", lambda req: (200, ITEMS[1]))
    html = signed_in.get("/timeline?day=2026-11-16").get_data(as_text=True)
    csrf = csrf_of(html)
    nxt = "/timeline?day=2026-11-16"

    r = signed_in.post(f"/timeline/{D1}/confirm", data={"csrf": csrf, "next": nxt})
    assert r.status_code == 302 and r.headers["Location"].endswith(nxt)
    assert backend.last("POST", rf"/api/v1/detections/{D1}:confirm")["body"] is None

    signed_in.post(f"/timeline/{D1}/dispute", data={"csrf": csrf, "next": nxt, "reason": "I was reviewing a PR"})
    assert backend.last("POST", rf"/api/v1/detections/{D1}:dispute")["body"] == {"reason": "I was reviewing a PR"}

    signed_in.post(f"/timeline/{D2}/claim", data={"csrf": csrf, "next": nxt, "day": "2026-11-16", "start": "10:30",
                                                  "end": "11:10", "area": "skill", "category": "reading",
                                                  "summary": "Reading the Drogon docs"})
    assert backend.last("POST", rf"/api/v1/detections/{D2}:claim")["body"] == {
        "area": "skill", "category": "reading", "summary": "Reading the Drogon docs",
        "started_at": "2026-11-16T10:30:00+05:30", "ended_at": "2026-11-16T11:10:00+05:30"}


def test_dispute_needs_a_reason_and_errors_are_shown(signed_in, backend):
    setup(backend)
    backend.on("POST", r"/api/v1/detections/[0-9a-f-]+:claim",
               lambda req: (409, {"title": "Conflict", "status": 409, "detail": "Already superseded."}))
    csrf = session_csrf(signed_in)
    r = signed_in.post(f"/timeline/{D1}/dispute", data={"csrf": csrf, "reason": "  "}, follow_redirects=True)
    assert "Say what is wrong" in r.get_data(as_text=True)
    assert not [c for c in backend.calls if c["path"].endswith(":dispute")]
    r = signed_in.post(f"/timeline/{D1}/claim", data={"csrf": csrf, "area": "work", "category": "x", "summary": "y",
                                                      "day": "2026-11-16", "start": "25:00", "end": "26:00"},
                       follow_redirects=True)
    assert "Times must be HH:MM" in r.get_data(as_text=True)
    r = signed_in.post(f"/timeline/{D1}/claim", data={"csrf": csrf, "area": "work", "category": "x", "summary": "y"},
                       follow_redirects=True)
    assert "Already superseded" in r.get_data(as_text=True)
    assert backend.last("POST", rf"/api/v1/detections/{D1}:claim")["body"] == {
        "area": "work", "category": "x", "summary": "y"}  # times left out: the detected ones stay


def test_an_end_before_the_start_is_the_next_day(signed_in, backend):
    setup(backend)
    backend.on("POST", r"/api/v1/detections/[0-9a-f-]+:claim", lambda req: (201, ITEMS[1]))
    signed_in.post(f"/timeline/{D1}/claim", data={"csrf": session_csrf(signed_in), "area": "personal",
                                                  "category": "sleep", "summary": "Slept", "day": "2026-11-16",
                                                  "start": "23:30", "end": "01:00"})
    body = backend.last("POST", rf"/api/v1/detections/{D1}:claim")["body"]
    assert body["started_at"] == "2026-11-16T23:30:00+05:30" and body["ended_at"] == "2026-11-17T01:00:00+05:30"


def test_collectors_that_went_quiet_are_flagged(signed_in, backend):
    now = datetime.now(timezone.utc)
    setup(backend, items=[], devices=[
        {"id": D1, "name": "Desk", "platform": "linux-x11", "collector_version": "0.0.1", "revoked_at": None,
         "last_seen_at": (now - timedelta(seconds=30)).isoformat(),
         "status": {"queue_depth": 3, "x_ok": True, "paused_until": None, "last_capture_at": None, "last_error": None}},
        {"id": D2, "name": "Laptop", "platform": "linux-x11", "collector_version": None, "revoked_at": None,
         "last_seen_at": (now - timedelta(hours=2)).isoformat(), "status": {}},
        {"id": D3, "name": "Old", "platform": "linux-x11", "revoked_at": now.isoformat(), "last_seen_at": None}])
    html = signed_in.get("/timeline").get_data(as_text=True)
    assert "Desk" in html and "Laptop" in html and "Old" not in html
    assert html.count('class="tag warn">silent') == 1
    assert "No detections for this day" in html


def test_ids_and_verbs_are_checked(signed_in, backend):
    csrf = session_csrf(signed_in)
    assert signed_in.post("/timeline/not-an-id/confirm", data={"csrf": csrf}).status_code == 404
    assert signed_in.post(f"/timeline/{D1}/delete", data={"csrf": csrf}).status_code == 404
    assert signed_in.get("/timeline?day=yesterday").status_code == 400
    assert not [c for c in backend.calls if "detections" in c["path"]]


def test_open_redirect_through_next_is_refused(signed_in, backend):
    backend.on("POST", r"/api/v1/detections/[0-9a-f-]+:confirm", lambda req: (200, ITEMS[1]))
    r = signed_in.post(f"/timeline/{D1}/confirm", data={"csrf": session_csrf(signed_in),
                                                        "next": "https://evil.example/"})
    assert r.headers["Location"].endswith("/timeline")
