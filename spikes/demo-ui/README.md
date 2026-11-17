# Demo UI (retired)

> **Retired on 2026-11-17.** The React web UI in `frontend/` replaced it with no loss of function (the parity gate,
> `frontend/scripts/parity.mjs`, checks that every user operation is called). It is kept here for reference only:
> `scripts/demo.sh` no longer starts it, CI no longer runs it, and it will drift from the contract.

A thin Flask client for exercising the backend while the real frontend (Stage 5) does not exist
(`docs/prelim_spec.md` §8). It contains no business logic: every page is a call to the public API.

## Run

```sh
python3 -m venv build/demo-venv && build/demo-venv/bin/pip install -r demo-ui/requirements.txt
cd demo-ui && THELIFE_API_URL=http://127.0.0.1:8080 ../build/demo-venv/bin/python -m demo_ui
# open http://127.0.0.1:5000 and sign in with an account made by thelife-admin
```

| Variable | Default | Meaning |
|---|---|---|
| `THELIFE_API_URL` | `http://127.0.0.1:8080` | Backend base URL |
| `THELIFE_CONTRACT` | `api/generated/openapi.yaml` | Contract the pages are generated from |
| `THELIFE_STAGE` | `4` | Hide operations of later stages |
| `DEMO_PORT` | `5000` | Listen port (always 127.0.0.1) |
| `DEMO_SECURE_COOKIES` | `0` | Set to `1` behind HTTPS |

## How it works

- **Generated from the contract.** Every `*.list` operation a user may call at the configured stage becomes a
  resource with list (filters from the query parameters), create, edit, delete/revoke and action pages. Forms
  are built from the request schemas. A new resource in `api/resources/` shows up without UI code.
- **Hand-written pages:** login, the calendar (with done/skip for routine occurrences), chat, settings, the
  detection timeline, and three panels (task occurrences, detection history, conversation transcript).
- **Timeline** (`/timeline`, Stage 4): one day (from the user's day start) of detections from the collector, the
  vision model and the user, in time order, with Confirm, Dispute (with a reason) and Correct (a claim: area,
  category, summary, times) on each. Corrected detections are hidden unless asked for. Counted minutes per area
  merge overlapping detections and leave out disputed and low-confidence ones. Collectors show when they were
  last heard from (silent after 5 min), their queue and pause.
- **Chat** (`/chat`, Stage 2): today's web conversation with each tool call and its result folded under the
  assistant step, pending approvals with Approve/Reject, and the assistant's status (online, offline until the
  quota resets, or not configured). There is no JavaScript, so the page refreshes itself every second while a turn
  runs instead of using the SSE stream. Commands (`/help`, `/today`, `/done N`, `/approve N`, ...) work even
  when the assistant is offline.
- **Presentation only.** Times are shown and entered in the user's timezone and sent as RFC 3339 with offset.
  Updates send only changed fields as JSON Merge Patch with `If-Match`. Server 422 messages appear next to the
  fields.

## Security

- The API session token never reaches the browser. It stays in an in-process session store, and the browser
  holds a random id in an `HttpOnly`, `SameSite=Strict` cookie. Logout revokes the token on the server.
  Restarting the UI signs everyone out.
- Every POST carries a CSRF token: the session's own, or a double-submit cookie on the login form.
- Device and API-key tokens are shown once, on an uncached page, and never stored.
- Strict CSP (no scripts at all), `nosniff`, `no-referrer`, `no-store`. `?next=` accepts only local paths. Ids
  are checked as UUIDs before they go into an API path.
- It listens on 127.0.0.1 only. It is a development tool, not something to expose.

## Tests

```sh
cd demo-ui && ../build/demo-venv/bin/python -m pytest -q
```

- `test_contract.py`, `test_forms.py`, `test_app.py` run against a fake backend (a `requests` transport adapter).
- `test_e2e_live.py` drives the real backend. It runs when `THELIFE_DEMO_E2E_API`, `THELIFE_DEMO_E2E_EMAIL` and
  `THELIFE_DEMO_E2E_PASSWORD` are set; CI does this in the backend workflow against a throwaway tenant.

If ROS or another system `PYTHONPATH` leaks pytest plugins in, run with
`env -u PYTHONPATH PYTEST_DISABLE_PLUGIN_AUTOLOAD=1`.
