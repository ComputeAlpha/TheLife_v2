from conftest import USER, csrf_of, session_csrf

TASK = {"id": "0190a000-0000-7000-8000-0000000000aa", "title": "Stretch", "area": "fitness", "status": "open",
        "version": 3, "priority": 1, "notes": None, "due_at": None, "rrule": "FREQ=DAILY",
        "rrule_dtstart": "2026-11-01T00:00:00+00:00", "day_profiles": [], "track_consistency": False,
        "created_at": "2026-11-01T00:00:00+00:00", "updated_at": "2026-11-01T00:00:00+00:00"}


def test_pages_require_login(client):
    r = client.get("/r/tasks")
    assert r.status_code == 302 and "/login" in r.headers["Location"]


def test_login_keeps_token_server_side(client, backend, app):
    page = client.get("/login").get_data(as_text=True)
    bad = client.post("/login", data={"email": "me@example.test", "password": "nope", "csrf": csrf_of(page)})
    assert bad.status_code == 401 and "Wrong email or password" in bad.get_data(as_text=True)
    r = client.post("/login", data={"email": "me@example.test", "password": "correct horse", "csrf": csrf_of(page)})
    cookie = r.headers.get_all("Set-Cookie")
    assert any(c.startswith("tl_demo=") and "HttpOnly" in c and "SameSite=Strict" in c for c in cookie)
    assert not any("tlu_secret" in c for c in cookie)
    assert len(app.extensions["demo"]["store"]) == 1


def test_login_requires_csrf_cookie(client):
    r = client.post("/login", data={"email": "me@example.test", "password": "correct horse", "csrf": "forged"})
    assert r.status_code == 400


def test_open_redirect_is_refused(client):
    page = client.get("/login").get_data(as_text=True)
    r = client.post("/login", data={"email": "me@example.test", "password": "correct horse", "csrf": csrf_of(page),
                                    "next": "//evil.example/x"})
    assert r.headers["Location"] == "/"


def test_posts_without_session_csrf_are_rejected(signed_in, backend):
    r = signed_in.post("/r/tasks/new", data={"title": "x", "area": "work"})
    assert r.status_code == 400
    assert not any(c["method"] == "POST" and c["path"] == "/api/v1/tasks" for c in backend.calls)


def test_security_headers(signed_in, backend):
    backend.on("GET", r"/api/v1/calendar", lambda req: (200, {"items": []}))
    r = signed_in.get("/")
    assert "default-src 'none'" in r.headers["Content-Security-Policy"]
    assert r.headers["Cache-Control"] == "no-store" and r.headers["X-Content-Type-Options"] == "nosniff"


def test_calendar_window_uses_user_day_start(signed_in, backend):
    backend.on("GET", r"/api/v1/calendar", lambda req: (200, {"items": [
        {"kind": "task_occurrence", "id": TASK["id"], "title": "Stretch", "starts_at": "2026-11-12T22:30:00+00:00",
         "ends_at": "2026-11-13T22:30:00+00:00", "all_day": True, "area": "fitness", "status": "todo",
         "occurs_on": "2026-11-13"}]}))
    html = signed_in.get("/calendar?start=2026-11-13&days=1").get_data(as_text=True)
    q = backend.last("GET", "/api/v1/calendar")["query"]
    assert q == {"from": "2026-11-13T04:00:00+05:30", "to": "2026-11-14T04:00:00+05:30"}
    assert f"/tasks/{TASK['id']}/occurrences/2026-11-13/complete" in html


def test_create_task_posts_contract_body(signed_in, backend):
    backend.on("POST", r"/api/v1/tasks", lambda req: (201, {**TASK, **req["body"]}))
    backend.on("GET", r"/api/v1/(goals|work/projects)", lambda req: (200, {"items": [], "next_cursor": None}))
    form = signed_in.get("/r/tasks/new").get_data(as_text=True)
    assert 'name="title"' in form and 'name="rrule"' in form
    r = signed_in.post("/r/tasks/new", data={"csrf": csrf_of(form), "title": "Stretch", "area": "fitness",
                                             "priority": "1", "status": "open", "due_at": "2026-11-20T18:00:00"})
    assert r.status_code == 302
    body = backend.last("POST", "/api/v1/tasks")["body"]
    assert body["due_at"] == "2026-11-20T18:00:00+05:30" and body["title"] == "Stretch"
    assert backend.last("POST", "/api/v1/tasks")["headers"]["Authorization"] == "Bearer tlu_secret"


def test_validation_errors_are_shown_on_fields(signed_in, backend):
    backend.on("POST", r"/api/v1/tasks", lambda req: (422, {"title": "Validation failed", "status": 422,
                                                            "errors": [{"path": "/rrule", "message": "can never occur"}]}))
    backend.on("GET", r"/api/v1/(goals|work/projects)", lambda req: (200, {"items": [], "next_cursor": None}))
    form = signed_in.get("/r/tasks/new").get_data(as_text=True)
    r = signed_in.post("/r/tasks/new", data={"csrf": csrf_of(form), "title": "x", "area": "work", "rrule": "FREQ=YEARLY"})
    html = r.get_data(as_text=True)
    assert r.status_code == 422 and "can never occur" in html and 'value="FREQ=YEARLY"' in html


def test_update_sends_if_match_and_only_changes(signed_in, backend):
    backend.on("GET", r"/api/v1/tasks/[0-9a-f-]+", lambda req: (200, TASK))
    backend.on("GET", r"/api/v1/tasks/[0-9a-f-]+/occurrences", lambda req: (200, {"items": []}))
    backend.on("GET", r"/api/v1/(goals|work/projects)", lambda req: (200, {"items": [], "next_cursor": None}))
    backend.on("PATCH", r"/api/v1/tasks/[0-9a-f-]+", lambda req: (200, {**TASK, **req["body"], "version": 4}))
    page = signed_in.get(f"/r/tasks/{TASK['id']}").get_data(as_text=True)
    form = {"csrf": csrf_of(page), "version": "3", "title": "Stretch more", "area": "fitness", "priority": "1",
            "status": "open", "rrule": "FREQ=DAILY", "rrule_dtstart": "2026-11-01T05:30:00"}
    assert signed_in.post(f"/r/tasks/{TASK['id']}/edit", data=form).status_code == 302
    patch = backend.last("PATCH", r"/api/v1/tasks/.*")
    assert patch["headers"]["If-Match"] == '"3"' and patch["body"] == {"title": "Stretch more"}
    assert patch["headers"]["Content-Type"] == "application/merge-patch+json"


def test_stale_version_shows_conflict(signed_in, backend):
    backend.on("GET", r"/api/v1/tasks/[0-9a-f-]+", lambda req: (200, TASK))
    backend.on("GET", r"/api/v1/(goals|work/projects)", lambda req: (200, {"items": [], "next_cursor": None}))
    backend.on("PATCH", r"/api/v1/tasks/[0-9a-f-]+", lambda req: (412, {"title": "Precondition Failed", "status": 412}))
    r = signed_in.post(f"/r/tasks/{TASK['id']}/edit",
                       data={"csrf": session_csrf(signed_in), "version": "2", "title": "x", "area": "fitness"},
                       follow_redirects=False)
    assert r.status_code == 302


def test_issued_token_is_shown_once_and_not_kept(signed_in, backend, app):
    key = {"id": "0190a000-0000-7000-8000-0000000000bb", "name": "ci", "scopes": ["tasks:read"], "version": 1,
           "token_prefix": "tlk_abcd"}
    backend.on("POST", r"/api/v1/api-keys", lambda req: (201, {"api_key": key, "token": "tlk_abcd_SECRET"}))
    form = signed_in.get("/r/api_keys/new").get_data(as_text=True)
    assert 'value="tasks:read"' in form and 'value="devices:write"' not in form
    r = signed_in.post("/r/api_keys/new", data={"csrf": csrf_of(form), "name": "ci", "scopes": "tasks:read"})
    assert r.status_code == 200 and "tlk_abcd_SECRET" in r.get_data(as_text=True)
    assert r.headers["Cache-Control"] == "no-store"
    assert backend.last("POST", "/api/v1/api-keys")["body"] == {"name": "ci", "scopes": ["tasks:read"]}
    for s in app.extensions["demo"]["store"]._items.values():
        assert "tlk_abcd_SECRET" not in repr(s)


def test_backend_401_ends_the_session(signed_in, backend, app):
    backend.on("GET", r"/api/v1/.+", lambda req: (200, {"items": [], "next_cursor": None}))
    backend.on("GET", r"/api/v1/tasks", lambda req: (401, {"title": "Unauthorized", "status": 401}))
    r = signed_in.get("/r/tasks")
    assert r.status_code == 302 and "/login" in r.headers["Location"]
    assert len(app.extensions["demo"]["store"]) == 0


def test_logout_revokes_backend_session(signed_in, backend, app):
    signed_in.post("/logout", data={"csrf": session_csrf(signed_in)})
    assert backend.last("POST", "/api/v1/auth/logout")["headers"]["Authorization"] == "Bearer tlu_secret"
    assert len(app.extensions["demo"]["store"]) == 0


def test_ids_are_validated_before_reaching_the_api(signed_in, backend):
    assert signed_in.get("/r/tasks/..%2F..%2Fme").status_code == 404
    assert signed_in.get("/r/nope").status_code == 404


def test_every_resource_list_renders(signed_in, backend, app):
    backend.on("GET", r"/api/v1/.+", lambda req: (200, {"items": [], "next_cursor": None}))
    for prefix in app.extensions["demo"]["contract"].resources:
        r = signed_in.get(f"/r/{prefix}")
        assert r.status_code == 200, prefix
        if app.extensions["demo"]["contract"].resources[prefix].create:
            assert signed_in.get(f"/r/{prefix}/new").status_code == 200, prefix


def test_list_filters_are_converted(signed_in, backend):
    backend.on("GET", r"/api/v1/.+", lambda req: (200, {"items": [], "next_cursor": None}))
    signed_in.get("/r/activities?from=2026-11-12T09:00:00&area=work&source=")
    q = backend.last("GET", "/api/v1/activities")["query"]
    assert q == {"from": "2026-11-12T09:00:00+05:30", "area": "work"}
    assert USER["timezone"] == "Asia/Kolkata"


def test_truncated_calendar_says_so(signed_in, backend):
    backend.on("GET", r"/api/v1/calendar", lambda req: (200, {"items": [], "truncated": True}))
    assert "pick fewer days" in signed_in.get("/").get_data(as_text=True)
