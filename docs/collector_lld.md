# TheLife v2: Collector Low-Level Design

Status: Draft 2 (review round 2 applied, 2026-11-11); implemented in Stage 4c (2026-11-16), see §13. Builds on `prelim_spec.md`, `backend_lld.md` (§7.2 ingest contract, §7.4 backend pipeline) and `agent_lld.md` (§8 notifications). Items marked **(proposed)** await confirmation.

---

## 1. Role and Boundaries

The collector is a **headless, simple** process. It observes the X11 desktop session and reports raw facts to the backend over the public API.

| The collector does | The collector does not |
|---|---|
| Read active window, title, WM_CLASS, PID | Parse titles or classify apps |
| Measure idle time | Talk to the database |
| Snapshot the user's own processes | Call any LLM or hold LLM credentials |
| Capture monitors on the server-provided schedule | Store screenshots on disk, ever |
| Queue observations offline and upload them | Decide what counts toward a score |
| Fetch its config from the backend | Show any UI of its own (no tray, no window) |
| Show desktop notifications the backend sends (agent nudges) | Execute anything the backend sends: there are no commands, scripts, updates or file paths in the protocol |
| Make only outbound connections | Listen on any port or socket |

All interpretation (classification, title parsing, vision, detection derivation, budget) lives in the backend (decision A). Changing rules never requires a collector release.

Local configuration is limited to: backend URL, device token, optional TLS CA/pin, and local paths.

## 2. Runtime Shape

- Binary: `thelife-collector` (C++20). One process, three threads:
  1. **Main / X thread:** xcb event loop plus timers (window changes, idle sampling, process snapshots, scheduling captures).
  2. **Capture thread:** grabs and encodes images, hands them straight to the uploader in memory.
  3. **Network thread:** observation batch uploads, capture uploads, config sync, heartbeat.
- A fourth, light thread long-polls for notifications and shows them over D-Bus (§7.5).
- Threads communicate through bounded in-process queues. The SQLite queue holds only observations.
- Sub-commands:
  - `thelife-collector run`: daemon mode (systemd).
  - `thelife-collector enroll --url URL --token tld_…`: validates against `GET /devices/self/config`, then writes config and token files.
  - `thelife-collector status`: reads the local state file (last upload, queue depth, last error, paused-until).
  - `thelife-collector pause <duration>` / `resume`: local privacy pause **(decided)**. The backend cannot override it; it only sees the pause in the heartbeat.

## 3. Dependencies

| Concern | Choice |
|---|---|
| Notifications | sd-bus (libsystemd), calling `org.freedesktop.Notifications.Notify` on the session bus |
| X11 | libxcb + xcb-randr (monitors), xcb-screensaver (idle), xcb-shm (fast capture), xcb-res (window → PID fallback), xcb-ewmh |
| HTTP | libcurl (HTTP/2, TLS, multipart). Drogon would be overkill for a client |
| Queue | SQLite3 (WAL mode) |
| Image | libjpeg-turbo, JPEG q≈80 **(proposed)**: universally accepted by OpenAI-compatible vision endpoints, unlike WebP |
| JSON | jsoncpp (same as backend; shared schema test fixtures) |
| Tests | GoogleTest, Xvfb, xdotool/XTest |
| Build | Same CMake + vcpkg tree as the backend (`collector/` subproject) |

## 4. Deployment: systemd user service

The service has to be a **user** unit bound to the graphical session. A system unit cannot reach the X display without fragile `DISPLAY`/`XAUTHORITY` hacks.

```ini
# ~/.config/systemd/user/thelife-collector.service  (installed by package)
[Unit]
Description=TheLife collector
PartOf=graphical-session.target
After=graphical-session.target

[Service]
ExecStart=/usr/bin/thelife-collector run
Restart=on-failure
RestartSec=10
# hardening that does not break X access
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=%h/.local/state/thelife-collector
Nice=10
IOSchedulingClass=idle

[Install]
WantedBy=graphical-session.target
```

- The session must export `DISPLAY`/`XAUTHORITY` to the user manager (`dbus-update-activation-environment --systemd DISPLAY XAUTHORITY`). Ubuntu's Xorg session already does this; the installer verifies it.
- If the X connection dies (logout, X crash), the collector exits with code 3 and systemd restarts it when the session comes back.

### 4.1 Files
| Path | Mode | Content |
|---|---|---|
| `~/.config/thelife-collector/config.toml` | 0600 | `backend_url`, optional `ca_file` / `pin_sha256`, log level |
| `~/.config/thelife-collector/device.token` | 0600 | device token (`tld_…`) |
| `~/.local/state/thelife-collector/queue.db` | 0600 | observation queue |
| `~/.local/state/thelife-collector/state.json` | 0600 | status for `status` sub-command |

The collector refuses to start if the token file is group- or world-readable.
Ubuntu 24.04 ships systemd 255, and encrypted credentials for **user** units (`systemd-creds --user`) need 256. So plain 0600 files are used now, with a move to `LoadCredentialEncrypted=` once the platform allows it.

## 5. Probes

### 5.1 Window probe (event-driven)
- Listen for `PropertyNotify` on the root window (`_NET_ACTIVE_WINDOW`) and on the current active window (`_NET_WM_NAME`, `WM_NAME`).
- On every change, emit a `window` observation:
  ```json
  {"window_id": "0x3a0000f", "wm_class": ["code", "Code"], "title": "main.cc — TheLife_v2 — Visual Studio Code",
   "pid": 41233, "exe": "/usr/share/code/code", "monitor": 1, "fullscreen": false}
  ```
- PID comes from `_NET_WM_PID`, with an XRes client-id query as fallback. `exe` is read from `/proc/<pid>/exe`.
- Debounce: a title that changes several times within 2 s produces only the last value (browsers with animated titles).
- Raw title is sent as is. Parsing happens server-side.

### 5.2 Idle probe (sampled)
- Every 5 s, `xcb_screensaver_query_info` → `ms_since_user_input`.
- Emit `idle` observations only on transitions: `idle_start` once input is older than `idle_threshold_s` (server config, default 120), backdated to the last input time; `idle_end` on the next input.
- Screen-lock state (screensaver active or a known locker in the foreground) is reported as `locked: true`, and it suspends captures.
- Media exception: a fullscreen window with no input stays "passive", not idle (video or lecture). It is reported as `idle_start{passive:true}` and the backend decides.

### 5.3 Process snapshot (periodic)
- Every `process_interval_s` (server config, default 60): scan `/proc`, keep only processes where **uid = session user**, drop kernel threads (`ppid == 2` or empty cmdline).
- Aggregate by executable: `{exe, comm, pids, cpu_pct (delta since last), rss_mb, has_window, focused}`.
- Cmdline arguments are **not** sent by default, only `argv[0]`. An args allowlist (apps whose args carry signal, for example an editor's `--folder-uri`) exists **only in the local `config.toml`**. The server cannot widen it, because arguments may carry secrets.
- Send a full snapshot every 10 min and deltas (`started[]`, `exited[]`, changed cpu) in between **(proposed)**, to keep the volume small.
- Classification (browser, editor, game, …) and noise filtering beyond the uid/kthread cut are backend rules (`backend_lld.md` §7.4).

### 5.4 Screen capture (scheduled)
- Monitors come from RandR `GetMonitors` (handles xrandr `--setmonitor` and multiple outputs).
- Grab with `xcb_shm_get_image` on the root window per monitor rectangle, falling back to `xcb_get_image`.
- Downscale so the long side is at most `capture_max_px` (server config, default 1600), then encode JPEG in memory.
- `capture_monitors: all | active` (server config, default `all` per the spec). The active-window monitor is flagged in the context.
- Buffers are `std::vector<uint8_t>`. They are zeroed and released as soon as the upload request finishes, whether it succeeds or fails.

## 6. Scheduling

The backend sends `capture_interval_s`, `jitter_pct`, `capture_monitors`, `capture_max_px`, `idle_threshold_s`, `process_interval_s` and `enabled` through `GET /devices/self/config`. The collector checks that config every 5 min with `If-None-Match`, and also immediately after a `409 config-stale` response.

Capture is skipped when any of these is true:
- user is idle (non-passive) or the screen is locked
- local pause active, or server `enabled=false` (budget exhausted, kill switch)
- backend unreachable (see §7)

Context-change trigger **(proposed)**: when the focused app changes and stays for more than 30 s, a capture may be pulled forward. A token bucket sized to the interval ensures this never exceeds the budgeted rate.

Each skip is reported as a `capture_meta` observation `{skipped: reason}`, so the backend can tell an empty timeline from an unobserved one.

## 7. Network and Offline Behaviour

### 7.1 Observations (durable)
- Every observation gets a monotonically increasing `client_seq` (persisted in SQLite) and is inserted in the same transaction.
- The uploader sends batches of up to 500 to `POST /ingest/observations`, with `Idempotency-Key = <device_id>:<first_seq>-<last_seq>`. Rows are deleted only for seqs the backend acknowledged (accepted or duplicate).
- Backoff: exponential, 2 s → 5 min cap, full jitter. `401` stops uploads and logs "token revoked" (no retry storm). `429`/`503` honour `Retry-After`.
- Queue cap: 7 days or 200k rows **(proposed)**. Beyond that the oldest rows are dropped and a `gap` observation is recorded.

### 7.2 Captures (never durable)
- Captures are uploaded immediately (`POST /ingest/captures`, multipart: `context` JSON part + one `image/jpeg` part per monitor).
- If the backend is unreachable or returns `503`, the capture is **dropped**, not queued, because screenshots may never be stored. A `capture_meta{skipped:"offline"|"backend_busy"}` observation is queued instead.
- Upload timeout is 30 s. There is no retry, because the next scheduled capture replaces it.

### 7.3 Heartbeat
- `POST /devices/self/heartbeat` every 60 s with `{version, queue_depth, paused_until, x_ok, last_capture_at, last_error}`. The backend surfaces "collector silent" when heartbeats stop.

### 7.4 Transport security
- HTTPS required except for `http://127.0.0.1` / `localhost` (same-machine topology).
- Optional `ca_file` for a private CA and optional `pin_sha256` public-key pinning (libcurl `CURLOPT_PINNEDPUBLICKEY`).

### 7.5 Desktop notifications
- `GET /devices/self/notifications?wait=55` (long poll over the same outbound HTTPS connection) returns `[{id, title, body, urgency: low|normal|critical, action: "open_chat"|"open_approval"|null, ref_id}]`, or `204` on timeout.
- The collector shows them through `org.freedesktop.Notifications.Notify`. Title and body are treated as plain text: markup is escaped and lengths are capped (title 80, body 400 chars).
- Clicking a notification opens the browser at a URL **built locally** from the configured `web_ui_url` + a fixed route + `ref_id` (validated as a UUID). The backend never supplies a URL.
- `POST /devices/self/notifications/{id}:ack {shown_at, clicked}` lets the backend stop delivering the same notification to other channels.
- While the user is idle or locked, notifications are held for up to 10 min and then dropped (the backend falls back to Telegram, `agent_lld.md` §8).

## 8. Security of the Backend → Collector Interface

The threat this section addresses: a compromised or impersonated backend turning the collector into a remote-control or surveillance backdoor.

| Rule | How it is enforced |
|---|---|
| No inbound surface | The collector never binds a socket. Everything is client-initiated HTTPS to `backend_url` |
| Inert protocol | The backend can send only (a) config values and (b) notification text. No command, code, script, update, file path or URL fields exist in any response schema |
| Strict parsing | Responses are validated against bundled JSON Schemas; unknown fields are ignored; size cap 256 KiB per response, max JSON depth 16. Fuzz-tested (libFuzzer) |
| Config is clamped locally | Hard local bounds that no server value can exceed: `capture_interval_s ≥ 60`, `capture_max_px ≤ 2560`, `process_interval_s ≥ 15`, `idle_threshold_s` 30–1800. Out-of-range values are clamped and logged |
| No on-demand capture | Captures come only from the local scheduler. No endpoint or field triggers an immediate capture |
| Local overrides win | Local pause and the local args allowlist cannot be changed by the server |
| Pinned endpoint | `CURLOPT_FOLLOWLOCATION` off (no redirects), fixed path list under `backend_url`, TLS 1.3 required for remote hosts, optional SPKI pinning; plain HTTP only for loopback |
| Token hygiene | Device token is scoped to `ingest:write devices:self notifications:read`. `POST /devices/self/token:rotate` issues a new token (old one valid 5 min), and the collector rotates every 30 days. Token never appears in logs or in the process command line |
| Least privilege at runtime | systemd user unit hardening (§4) plus `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`, `SystemCallFilter=@system-service`, `MemoryDenyWriteExecute=yes`, `LockPersonality=yes` |
| Audit on both sides | Every config change the collector applies is logged locally with the old and new values; the backend logs every config served |

The backend side mirrors this: device tokens can reach only the ingest, config, heartbeat and notification endpoints, enforced by scope checks and covered by API tests that call every other route with a device token and expect 403.

## 9. Source Layout

```
collector/
  CMakeLists.txt
  src/
    main.cc                 # sub-command dispatch
    app/                    # daemon wiring, signals (SIGTERM → flush & exit), state file
    config/                 # toml + server config cache, validation
    x11/                    # connection, atoms, ewmh helpers, event loop integration
    probes/                 # window, idle, process
    capture/                # randr monitors, shm grab, scale, jpeg
    sched/                  # interval + jitter, skip rules, token bucket
    queue/                  # sqlite store, client_seq
    net/                    # curl wrapper, uploader, config sync, heartbeat, token rotation
    notify/                 # long-poll client, sd-bus Notify, local URL builder
  packaging/
    systemd/thelife-collector.service
    debian/                 # later
  tests/
    unit/  x11/  e2e/
    fixtures/synthetic_window.cc   # tiny xcb app: set title/class/pid on command
```

Interfaces for testing: `IClock`, `IXSource`, `IProcFs` (rooted at a fake `/proc` directory), `IHttp`. Probes and scheduler depend on these, never on globals.

## 10. Observation Payload Schemas (`api/schemas/observation.*.json`)

| kind | payload |
|---|---|
| `window` | `window_id, wm_class[2], title, pid, exe, monitor, fullscreen` |
| `idle` | `event: start|end, at, passive, locked` |
| `process_snapshot` | `full: bool, procs[]` or `started[], exited[], changed[]` |
| `capture_meta` | `capture_id?, skipped?, monitors, active_monitor, duration_ms` |
| `gap` | `from_seq, to_seq, reason` |
| `session` | `event: start|end, collector_version, x_screen, monitors[]` |

These schemas are shared with the backend (one source of truth, validated on ingest).

## 11. Testing

| Suite | What | How |
|---|---|---|
| unit | scheduler skip rules, token bucket, backoff, queue ack/delete, process aggregation and delta, idle transitions | fake clock, fake `/proc` tree, fake HTTP |
| x11 | window probe follows `_NET_ACTIVE_WINDOW` and title changes; RandR with 2 monitors; capture dimensions and scaling; idle via XTest input | `Xvfb :99 -screen 0 3840x1080x24` + `xrandr --setmonitor` to split into 2 monitors; synthetic window fixture; test sets `_NET_ACTIVE_WINDOW` itself (no WM needed) |
| offline | backend down → observations accumulate, captures dropped with `capture_meta`, recovery drains queue without duplicates | fake HTTP server toggled up and down |
| notifications | long-poll, markup escaped, URL built locally only, held while idle, ack sent | fake HTTP + a test D-Bus session (`dbus-run-session`) with a stub notification server |
| hostile backend | oversized/deep JSON, unknown fields, out-of-range config, redirects, fields resembling commands or URLs: all rejected or clamped | fake HTTP server + libFuzzer targets for config and notification parsers |
| security | refuses world-readable token; no file written under state dir other than queue/state; no JPEG bytes ever reach disk (inotify watch on `$HOME` and `/tmp` during capture tests) | |
| e2e | capture → backend → fake LLM → `detections(source=vision)`, and window spans → `detections(source=process)` | real backend + Postgres + fake OpenAI server, collector under Xvfb |

## 12. Decisions (review round 2, 2026-11-11)

1. **Queue at rest:** 0600 user-owned SQLite file is sufficient; no queue encryption.
2. **Screenshot exclusions:** none for now. The effort goes into hardening the backend → collector interface instead (§8).
3. **Local pause:** kept.
4. **Desktop notifications:** the collector delivers agent notifications (§7.5), with the protocol kept inert.
5. **Browser URL capture:** still out of scope.

## 13. Implementation Notes (Stage 4c, 2026-11-16)

The collector lives in `collector/` (C++20, same CMake and vcpkg tree; xcb and libsystemd from the system). What
the implementation settled, and where it departs from the sections above:

- **Threads.** The capture thread grabs, scales, encodes **and uploads**, on its own X connection and HTTP handle,
  so pixels and JPEG bytes never cross a thread boundary (§2 had the network thread upload them). The network
  thread does observation batches, config sync, heartbeat and token rotation. Notifications use two threads: one
  long-polls the backend, one owns the sd-bus connection (shows notifications, handles clicks). sd-bus is not
  thread-safe, and a 55 s long-poll cannot share a thread with the bus.
- **Enrolment reads the token from standard input** (no echo on a terminal): `thelife-collector enroll --url URL`
  and the token pasted or piped in. A `--token` argument is refused, per §8 ("never in the process command line").
- **client_seq.** SQLite `AUTOINCREMENT`, and a new queue file starts at the current Unix time in microseconds, so
  a deleted queue cannot collide with sequences the backend already stored (it deduplicates on device and seq).
  Idempotency-Key is `<install id>:<first>-<last>`: the device id is not known to the collector, and a random
  install id (queue metadata) is just as unique.
- **Pause** lives in the queue's metadata table, so the `pause` command and the daemon never write the same file. A
  pause stops **all** observation (windows, idle, processes, captures), not only captures; the heartbeat still
  reports `paused_until`. On resume the focused window is reported again.
- **Process deltas are always sent**, an empty `{full:false}` included: the backend's focus spans end after five
  minutes without any observation (backend_lld §7.4), so a quiet minute must still say "alive".
- **Locked screen**: the X screensaver state, a few lockers recognised by window class, and logind's `LockedHint`
  for the user's graphical session (`/org/freedesktop/login1/user/self` → `Display`; a user service is not inside
  a session, so `session/auto` does not work).
- **No focused window** is reported as `window_id 0x0` with an empty class and title. It ends the previous focus
  span cleanly and classifies as nothing.
- **Captures.** MIT-SHM with a private SysV segment (removed right after attach, zeroed before detach), falling
  back to GetImage for a remote display. Area-average downscale, JPEG q80 4:2:0 into a pre-sized buffer
  (`TJFLAG_NOREALLOC`), multipart parts streamed from that buffer by a read callback (libcurl does not copy them).
  Every pixel and JPEG buffer is a `SecureBytes`, zeroed on release. The process is not dumpable
  (`PR_SET_DUMPABLE 0`): no core files with screen contents, no ptrace by other processes of the user.
- **First capture** comes 60 s after start, then every `capture_interval_s` ± jitter. While capture is off or the
  config is unknown, the due capture is recorded as skipped (`disabled`) and the next one is a full interval away.
  The context-change trigger pulls a capture forward when a newly focused app stays 30 s, and only if a full
  interval has passed since the last capture (a token bucket of one).
- **Rejected uploads.** A 422 drops the rows the problem's `errors[].path` points at (`/observations/<i>/…`), or
  the whole batch when none is named, and records a `gap{reason: corrupt}`, so one bad row cannot wedge the queue.
- **Revoked token.** On 401 uploads stop (no retry storm); the daemon re-reads the token file every minute and
  resumes when a re-enrolment wrote a new one.
- **Notifications.** Held up to 10 min while the user is idle or locked, then dropped unacknowledged (Telegram
  takes over). A click opens `<web_ui_url>/chat` (or `/chat#approval-<uuid>`) through `xdg-open`; the URL is built
  only from local configuration and a UUID-validated `ref_id`. Without a session bus, notifications are off and
  the long-poll does not run.
- **Not used:** xcb-ewmh and xcb-icccm (the few EWMH atoms are read directly).
- **Fuzzing** (`collector/fuzz/`): libFuzzer + ASan/UBSan targets for the config response, the notification
  response (and what is shown and opened from it) and `config.toml`, each with invariants (local bounds hold, no
  markup survives, click URLs are only the local web UI plus a UUID, what enroll writes reads back the same).
  `scripts/fuzz.sh [seconds]` builds them with clang-18 (`cmake --preset fuzz`); the first run, 2 min per target
  (about 15 million inputs), found nothing. In the normal build the same targets replay `collector/fuzz/corpus/`
  as tests, so a saved crash input stays a regression test.
- **Test-only environment** (local, never from the server): `THELIFE_COLLECTOR_FAST_TIMERS=1` shortens the local
  timers (first capture 2 s, config 3 s, heartbeat 2 s, backoff cap 3 s) but not the §8 bounds;
  `THELIFE_COLLECTOR_NO_LOGIND=1`; `THELIFE_COLLECTOR_PROC_ROOT`; `THELIFE_COLLECTOR_CONFIG_DIR` / `_STATE_DIR`.
- **Tests** (`ctest -L collector`): unit (fake HTTP, fake X, fake `/proc`; every emitted payload is checked against
  `api/schemas/observations.json`); system (Xvfb split into two RandR monitors, the real binary against
  `collector/tests/fixtures/fake_backend.py`: offline and recovery, 409/503, hostile backend, pause, rotation,
  revoked token, notifications over a private `dbus-daemon` with a stub notification server, inotify proof that no
  JPEG bytes reach disk); e2e (the real backend and test database: a capture becomes a vision detection,
  observations are stored, the token rotates; and, labelled `slow`, 150 s of focus on a synthetic editor window
  becomes the process detection "VS Code: TheLife_v2", which is then corrected with a claim).

