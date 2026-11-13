"""Chat page (Stage 2): transcript with folded tool steps, sending with a client id, refresh while a turn runs,
approvals decided from the page, assistant status."""
from conftest import session_csrf

CONV = "0190a000-0000-7000-8000-0000000000c1"
TURN = "0190a000-0000-7000-8000-0000000000d1"
APPROVAL = "0190a000-0000-7000-8000-0000000000e1"
MESSAGES = [
    {"id": "m1", "conversation_id": CONV, "role": "user", "kind": "normal", "content": "need to renew my passport",
     "tool_calls": [], "created_at": "2026-11-13T09:00:00+00:00"},
    {"id": "m2", "conversation_id": CONV, "role": "assistant", "kind": "normal", "content": "",
     "tool_calls": [{"id": "c1", "name": "tasks_create", "arguments": "{\"title\": \"Renew passport\"}"}],
     "created_at": "2026-11-13T09:00:01+00:00"},
    {"id": "m3", "conversation_id": CONV, "role": "tool", "kind": "normal", "content": "{\"id\": \"t1\"}",
     "tool_calls": [], "tool_call_id": "c1", "tool_name": "tasks_create", "created_at": "2026-11-13T09:00:01+00:00"},
    {"id": "m4", "conversation_id": CONV, "role": "assistant", "kind": "normal",
     "content": "Logged: task Renew passport.", "tool_calls": [], "created_at": "2026-11-13T09:00:02+00:00"},
]


def wire(backend, turn_status="done", mode="online", pending=()):
    backend.on("GET", r"/api/v1/conversations", lambda req: (200, {"items": [{"id": CONV}], "next_cursor": None}))
    backend.on("GET", rf"/api/v1/conversations/{CONV}/messages", lambda req: (200, {"items": MESSAGES}))
    backend.on("GET", rf"/api/v1/chat/turns/{TURN}", lambda req: (200, {
        "id": TURN, "conversation_id": CONV, "message_id": "m1", "status": turn_status}))
    backend.on("GET", r"/api/v1/system/llm-status", lambda req: (200, {
        "mode": mode, "resets_at": "2026-11-15T18:30:00Z" if mode == "manual" else None, "providers": [],
        "usage": {"weekly_request_estimate": 3000}}))
    backend.on("GET", r"/api/v1/approvals", lambda req: (200, {"items": list(pending), "next_cursor": None}))


def test_transcript_folds_tool_steps_and_shows_status(signed_in, backend):
    wire(backend)
    html = signed_in.get("/chat").get_data(as_text=True)
    q = backend.last("GET", "/api/v1/conversations")["query"]
    assert q["channel"] == "web" and q["from"] == q["to"]
    assert "need to renew my passport" in html and "Logged: task Renew passport." in html
    assert "<code>tasks_create</code>" in html and "&#34;Renew passport&#34;" in html  # args parsed, escaped
    assert "Assistant online" in html
    assert 'http-equiv="refresh"' not in html


def test_send_uses_a_client_id_and_refreshes_while_the_turn_runs(signed_in, backend):
    wire(backend, turn_status="running")
    backend.on("POST", r"/api/v1/chat/web/messages", lambda req: (202, {
        "id": TURN, "conversation_id": CONV, "message_id": "m1", "status": "queued"}))
    page = signed_in.get("/chat").get_data(as_text=True)
    import re
    client_id = re.search(r'name="client_id" value="([0-9a-f]+)"', page).group(1)
    r = signed_in.post("/chat", data={"text": "  did 45 min rust  ", "client_id": client_id, "csrf": session_csrf(signed_in)})
    assert r.status_code == 302 and r.headers["Location"].endswith(f"/chat?turn={TURN}")
    assert backend.last("POST", "/api/v1/chat/web/messages")["body"] == {"text": "did 45 min rust", "client_msg_id": client_id}
    html = signed_in.get(f"/chat?turn={TURN}").get_data(as_text=True)
    assert 'http-equiv="refresh" content="1"' in html and "working on it" in html


def test_manual_mode_and_approvals_on_the_page(signed_in, backend):
    approval = {"id": APPROVAL, "summary": "Update a task. Target: Task x (version 1).", "status": "pending",
                "expires_at": "2026-11-16T09:00:00+00:00", "tool": "tasks.update"}
    wire(backend, mode="manual", pending=[approval])
    backend.on("POST", rf"/api/v1/approvals/{APPROVAL}:reject", lambda req: (200, {**approval, "status": "rejected"}))
    html = signed_in.get("/chat").get_data(as_text=True)
    assert "Assistant offline until" in html and "commands only" in html
    assert "Update a task." in html
    r = signed_in.post(f"/chat/approvals/{APPROVAL}/reject", data={"reason": "not now", "csrf": session_csrf(signed_in)})
    assert r.status_code == 302
    assert backend.last("POST", rf"/api/v1/approvals/{APPROVAL}:reject")["body"] == {"reason": "not now"}
    assert "Rejected; nothing changed." in signed_in.get("/chat").get_data(as_text=True)
    assert signed_in.post("/chat/approvals/not-a-uuid/approve", data={"csrf": session_csrf(signed_in)}).status_code == 404


def test_empty_message_is_not_sent(signed_in, backend):
    wire(backend)
    r = signed_in.post("/chat", data={"text": "   ", "csrf": session_csrf(signed_in)})
    assert r.status_code == 302
    assert not any(c["path"] == "/api/v1/chat/web/messages" for c in backend.calls)
