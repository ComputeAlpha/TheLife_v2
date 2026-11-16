# TheLife v2: Backend Low-Level Design

Status: Draft 2. Builds on `prelim_spec.md`. Decisions from review round 1 are recorded in §13. Items still marked **(proposed)** await confirmation.
Scope: Stage 0 and Stage 1 in detail, with the Stage 2–4 seams (tool registry, jobs, collector ingest) fixed now so later stages do not reshape the core.

---

## 1. Repository Layout (monorepo) **(proposed)**

```
TheLife_v2/
  api/
    common.yaml, resources/   # source of truth: shared types + one file per resource (docs/tool_registry.md)
    generated/                # openapi.yaml, tools.json, agent_tools.json (generated, committed, CI-checked)
    tools/                    # contractgen.py, validate.py
  backend/
    CMakeLists.txt
    vcpkg.json
    config/                   # drogon config templates (dev, test, ci)
    migrations/               # NNNN_description.sql, forward-only
    src/
      main.cc
      core/                   # config, errors, ids (UUIDv7), time/tz, json helpers
      db/                     # pool, TenantTx, auth SECURITY DEFINER wrappers
      http/                   # filters: request-id, auth, scope, problem+json, idempotency
      auth/                   # tokens, argon2id passwords, principals
      tools/                  # registry, ToolDef, policy, schema validation, audit
      jobs/                   # job queue + worker loop
      modules/
        tasks/  calendar/  activities/  goals/  work/  skills/
        entertainment/  detections/  devices/  ingest/  ...
          <module>/{repo.*, service.*, tools.*, controller.*}
    tools/admin_cli/          # thelife-admin: tenant + user creation
    tests/
      unit/  integration/  api/  isolation/
  collector/                  # see collector_lld.md (next)
  demo-ui/                    # Flask, pure API client
  docker/                     # compose: postgres+pgvector, fake-llm
  .github/workflows/
```

## 2. Build and Dependencies

| Concern | Choice |
|---|---|
| Build | CMake ≥ 3.28, Ninja, C++20 (coroutines on) |
| Packages | **vcpkg manifest mode (decided)**. Drogon, libsodium, GTest, valijson all ship as ports; Conan offers no advantage here |
| HTTP/ORM | Drogon (`drogon[postgres]`), raw SQL through `DbClient`, **no Drogon ORM models (proposed)**. RLS and hand-written SQL are easier to reason about than generated mappers |
| JSON | jsoncpp (Drogon's native type). Avoid a second JSON library |
| JSON Schema | valijson with its jsoncpp adapter, used by the tool registry and request validation |
| Crypto | libsodium: argon2id passwords, CSPRNG tokens, `crypto_secretbox` for envelope encryption |
| LLM HTTP | libcurl (vcpkg, OpenSSL) for model calls only: streaming SSE bodies, which Drogon 1.9's `HttpClient` cannot deliver incrementally. Runs on the LLM gateway's own thread pool (`agent_lld.md` §3) |
| Recurrence | libical 3.0 (vcpkg) for RFC 5545 RRULE parsing and expansion (`core/recurrence`). Rules are expanded in **floating local time** and converted to instants by Postgres (`AT TIME ZONE`), so a 09:00 routine stays at 09:00 across DST. Accepted: `FREQ=DAILY\|WEEKLY\|MONTHLY\|YEARLY`, INTERVAL 1–1000; rules that can never occur are rejected. libical's fatal-error mode is switched off at first use |
| Tests | GoogleTest + real PostgreSQL 17 with pgvector from docker compose |
| Migrations | **dbmate (decided)**: plain SQL, one static binary, no ORM coupling |
| CI | GitHub Actions: build, unit, integration (service container), isolation, contract tests |

Async model: Drogon C++20 coroutines (`drogon::Task<>`, `co_await` on DB and HTTP clients) throughout. No blocking calls on event-loop threads.

Coroutine rules learned in Stage 1 (GCC 13/14):
- `TenantTx::exec(sql, args...)` copies its arguments into the coroutine frame and **rejects `std::optional` at compile time**. A nullable value passed through that parameter pack reached Postgres as `''` instead of NULL. Nullable parameters go through `TenantTx::execParams(sql, std::vector<std::optional<std::string>>)` with explicit casts in SQL.
- No braced initializer lists inside a `co_await` expression (internal compiler error); build the value first.

### 2.1 Embedding service (decided 2026-11-12, see `spikes/embed/REPORT.md`)
- A separate process, **`thelife-embedder`**, owns the GPU. The backend talks to it over a **Unix domain socket** (`$XDG_RUNTIME_DIR/thelife/embedder.sock`, mode 0660, owner-only directory), so a CUDA fault or OOM cannot take down the API and the backend binary carries no CUDA dependency.
- Protocol **(proposed)**: HTTP/1.1 over the socket (Drogon on both sides), `POST /v1/embed {texts[], max_tokens}` → `{model, model_version, dim, vectors[][]}`, `GET /v1/health` → provider (`cuda` required), model version, GPU memory. Batches ≤ 8, ≤ 512 tokens per text (chunking is the caller's job).
- Runtime: the official **ONNX Runtime 1.26.0 linux-x64-gpu (CUDA 12)** prebuilt release, fetched by CMake with a pinned SHA-256, and the fp16 attention-fused bge-m3 from `spikes/embed/convert_fp16.py`. Tokenization: vcpkg `sentencepiece` with the XLM-R id mapping and whitespace normalization.
- Startup fails unless `CUDAExecutionProvider` is active (guards against the silent CPU fallback found in the spike). A `--cpu` flag exists for development machines without a GPU and logs loudly.
- The backend treats the embedder as optional: if it is down, `embeddings.upsert` jobs back off and retrieval falls back to recency plus keyword. Tests use a fake embedder that serves deterministic vectors on a temp socket.
- **Implemented (Stage 2, increment 3b):** `backend/embedder/` (`thelife-embedder`, its tokenizer library and model
  wrapper). The ONNX Runtime 1.26.0 GPU archive is fetched by CMake with a pinned SHA-256 (`THELIFE_ORT_ARCHIVE` can
  point at a downloaded copy; the `ci` preset sets `THELIFE_BUILD_EMBEDDER=OFF`). The server is a small HTTP/1.1
  implementation over the Unix socket (Drogon cannot listen on one), one inference at a time, batch ≤ 8,
  ≤ 512 tokens. Measured on the RTX 3080 Ti: ~1.4 GB GPU memory, 6 texts in 33 ms including the round trip.
  Parity (`tests/embedder/embedder_test.cc`, against `tests/fixtures/embed/reference.json` from the upstream
  Hugging Face tokenizer and the fp32 model on the CPU): token ids identical for all 8 texts (English, Tamil,
  Hindi, Japanese, emoji, SQL, messy whitespace), fp16 CUDA vectors at cosine ≥ 0.999, batching does not change a
  vector. The backend has no CUDA dependency; `THELIFE_EMBEDDER_SOCKET` unset means keyword search only.
- **Sync instead of per-row jobs:** `embeddings.sync` (one per tenant, coalesced) embeds whatever is missing or
  changed: messages (90 days), activities, summaries and active facts, compared by `md5(text)` and model version,
  in chunks of ≤ 1,500 characters. It is queued when a turn finishes and after a day summary; while the embedder is
  down it is deferred, not failed.

## 3. Request Lifecycle

```
request
 → RequestIdFilter      assigns X-Request-Id, attaches it to the logger
 → AuthFilter           Bearer token → sha256 → auth.resolve_token() → Principal
 → ScopeFilter          route's required scopes ⊆ principal.scopes
 → Controller           parses + validates body against the JSON Schema
 → ToolRegistry.invoke  policy check → handler(ctx, args) → audit row
     handler opens TenantTx:
       BEGIN;
       SELECT set_config('app.tenant_id', $tenant, true),
              set_config('app.principal_kind', $kind, true),
              set_config('app.principal_id', $id, true);
       ... queries ...
       COMMIT;
 → ProblemJson mapper   exceptions → RFC 9457 application/problem+json
```

`Principal { tenant_id, kind: user|device|api_key|agent|system, id, user_id, scopes }`.
The agent acts as `kind=agent` for a given user, so the policy engine can tell agent actions from user actions.

Every DB access goes through `TenantTx`, so there is no way to run a query without a tenant context. A lint test greps for direct `DbClient::execSql` use outside `db/`.

## 4. Multi-Tenancy and RLS

### 4.1 Roles
| Role | Purpose |
|---|---|
| `thelife_owner` | Owns schema objects and runs migrations. Never used by the service |
| `thelife_app` | Service login role. `NOSUPERUSER NOBYPASSRLS`, not the table owner |
| `thelife_admin` | Used only by `thelife-admin` CLI for tenant/user bootstrap |

### 4.2 Policy pattern (every tenant table)
```sql
ALTER TABLE tasks ENABLE ROW LEVEL SECURITY;
ALTER TABLE tasks FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON tasks
  USING      (tenant_id = current_setting('app.tenant_id', true)::uuid)
  WITH CHECK (tenant_id = current_setting('app.tenant_id', true)::uuid);
```
If the setting is missing, `current_setting(..., true)` returns NULL, the predicate is NULL and no rows are visible. This satisfies "queries without a tenant context return nothing".

### 4.3 Pre-tenant lookups
Login and token resolution happen before the tenant is known. They go through `SECURITY DEFINER` functions in an `auth` schema, owned by `thelife_owner`, with a fixed `search_path`:
- `auth.resolve_token(hash bytea) → (tenant_id, kind, principal_id, user_id, scopes, expires_at)`
- `auth.find_login(email citext) → (tenant_id, user_id, password_hash, status)`
- `jobs.claim(worker text, kinds text[], n int)`: cross-tenant job claim (§10)
- `ops.telegram_chat_user(chat_id bigint) → (tenant_id, user_id)`: an incoming Telegram chat to its live link (active user and tenant only); `ops.telegram_link(code_hash, chat_id, username)`: consumes a one-time link code and links the chat (`agent_lld.md` §7.2). Owned by `thelife_definer`, executable by `thelife_app` only; covered by `api/telegram_test.cc`.

These are the only cross-tenant paths. Each has its own tests.

### 4.4 Catalog tests (isolation suite)
- Every table in `public` with a `tenant_id` column has RLS enabled, RLS forced and a `tenant_isolation` policy. The test fails if a new table lacks them.
- Generated leak test per table: seed tenants A and B, set context A, assert `count(*)` over B's ids = 0, and assert that inserting with `tenant_id = B` fails `WITH CHECK`.
- No context set → every table returns 0 rows.

### 4.5 pgvector + RLS (risk 4)
With one tenant the HNSW post-filter issue does not show up. Plan: pgvector ≥ 0.8 with `hnsw.iterative_scan = relaxed_order`, plus a benchmark test in Stage 2 with 2 synthetic tenants × 200k vectors. Fallback: list-partition `embeddings` by tenant.

## 5. Identifiers, Time, Common Columns

- **IDs:** UUIDv7 generated in the app (time-ordered, so B-tree friendly and usable as a cursor).
- **Time:** every instant is `timestamptz`, and the API uses RFC 3339 with offset. Each user has an IANA `timezone`.
- **Day boundary:** a user setting `day_start_hour` (default 4) **(decided)**, so 01:30 work counts toward the previous day. All daily scores, "today" views and recurrence materialization use `(timezone, day_start_hour)`.
- **Common columns** on domain tables:
  ```
  id uuid PK, tenant_id uuid NOT NULL,
  created_at, updated_at timestamptz,
  created_by_kind principal_kind NOT NULL,   -- user|agent|external|device|system
  created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', -- direct|user_stated|inferred|imported
  source_message_id uuid NULL,               -- chat message the item came from (agent_lld §7.4)
  version int NOT NULL DEFAULT 1,            -- optimistic concurrency (ETag)
  deleted_at timestamptz                     -- soft delete; hard delete only via retention jobs
  ```
  `created_by_kind` + `origin` drive the 5.3 rule "edit agent-created items: automatic; edit user-entered: approval". An item the agent created from something the user said (`origin=user_stated`) counts as user-entered.

## 6. Schema (Stage 1 tables; later-stage tables listed for shape only)

### 6.1 Identity and access
```
tenants(id, name, status, agent_writes_enabled bool,   -- kill switch
        settings jsonb, created_at)
users(id, tenant_id, email citext UNIQUE, display_name, password_hash,
      role user|tenant_admin, timezone, day_start_hour, status, ...)
credentials(id, tenant_id, kind user_session|device|api_key,
            user_id, device_id NULL, name, token_prefix char(8),
            token_hash bytea UNIQUE, scopes text[],
            expires_at, last_used_at, revoked_at, created_at)
devices(id, tenant_id, user_id, name, platform, hostname,
        collector_version, last_seen_at, config jsonb, revoked_at)
```
Tokens are 32 random bytes, base64url, with a kind prefix (`tlu_`, `tld_`, `tlk_`) so leaked tokens are easy to identify. Only the SHA-256 hash is stored. The plaintext is returned once, at creation.
- **Devices (implemented):** `POST /devices` creates the device and its `tld_` credential, scoped to `ingest:write devices:self notifications:read`. Revoking a device revokes its credentials, and `auth.authenticate` also rejects tokens of revoked devices. Revocation is idempotent.
- **API keys (implemented):** stored as `credentials(kind = api_key)` and served through the `api_keys` view (`security_invoker`, so the credentials RLS policy applies; `token_hash` is not exposed). A key may only hold scopes of tools that `api_key` principals can call, so it can never mint keys, manage devices or act as a collector. `expires_at` must be in the future.

### 6.2 Planning
```
goals(…common, title, description, area, horizon year|quarter|month|week,
      parent_id, target jsonb, starts_on, due_on, status)
plans(…common, goal_id NULL, title, body, period_start, period_end, status)
```
`area` enum: `work | skill | fitness | personal | entertainment | other`. It lines up with the scoring weights (5.7); entertainment is tracked but weighted 0 **(decided)**.

### 6.3 Tasks and calendar (implemented: migration 0004)
```
tasks(…common, title, notes, area, goal_id, project_id, priority 0-3,
      estimate_min, due_at, status open|done|cancelled, completed_at,
      rrule text NULL, rrule_dtstart timestamptz NULL,       -- recurring if rrule set
      day_profiles text[], track_consistency)
task_occurrences(…common, task_id, occurs_on date, status todo|done|skipped|missed,
                 completed_at, skip_reason, UNIQUE(tenant_id, task_id, occurs_on))
calendar_events(…common, title, notes, starts_at, ends_at, all_day,
                rrule, location, area, category, link_type, link_id)
```
- Recurrence is expanded on read for ranges. Occurrence rows are materialized lazily, either when the user or agent acts on one (`:complete`, `:skip`) or when the nightly job marks `missed`, so the table only holds occurrences that have a state.
- **Occurrence day:** the rule is expanded from `rrule_dtstart` in the user's timezone shifted back by `day_start_hour`, so a 02:00 session belongs to the previous day when the day starts at 04:00.
- Triggers keep derived columns honest: `completed_at` follows `status = done`, `rrule_dtstart` defaults to `now()` when a rule is set without one.
- `calendar.view` merges, for a window of at most 62 days (default: the next 7):
  - events, with recurring ones expanded;
  - open recurring tasks, as all-day entries per local day, with any stored occurrence status;
  - one-off tasks due in the window;
  - entertainment slots;
  - skill sessions.
- **Links** (`link_type`/`link_id` on events and activities) are checked by the `app.check_link` trigger: the target must be a live row of the named type. RLS makes another tenant's id look missing.

### 6.4 Activities, work, skills, entertainment (implemented: migrations 0004, 0005)
Tables are named after the contract prefix: `work_roles`, `work_responsibilities` (`cadence` is an RRULE), `work_projects`, `work_deliverables` (`delivered_at` set by trigger), `skill_resources`, `skill_sections`, `skill_sessions`, `entertainment_items`, `entertainment_slots`, `activities`.
```
activities(…common, area, category, title, started_at, ended_at,
           source manual|detection|agent, detection_id NULL, link_type, link_id, notes)
work_roles / work_responsibilities(role_id) / work_projects(role_id) / work_deliverables(project_id, due_at, status)
skill_resources(…common, kind book|course, title, author, url, total_pages, total_duration_s, goal_id, status,
                progress jsonb)                     -- derived, maintained by triggers
skill_sections(…common, resource_id, ordinal, title, duration_s)   -- ordinal unique per live resource
skill_sessions(…common, resource_id, started_at, ended_at,
               page_from, page_to,                 -- books
               section_id, pos_from_s, pos_to_s,    -- courses
               notes, source)
entertainment_items(…common, kind game|movie|series, title, meta jsonb, status)
entertainment_slots(…common, item_id, starts_at, ends_at, consumed bool)
```
- **Skill progress** is derived, never written by clients. Books: furthest `page_to`. Courses: furthest timeline position (sum of the durations of earlier sections plus `pos_to_s`). Percent uses `total_pages` / `total_duration_s`. Triggers on sessions and sections recompute the parent resource, which also bumps its version and ETag.
- Book sessions record pages only, course sessions record sections and positions only, and sections exist only on courses (trigger). A session's section must belong to its resource (composite FK).

### 6.5 Detections (append-only)
```
detections(id, tenant_id, created_at,
           source vision|process|user|external,
           device_id NULL, observation_ids uuid[],
           started_at, ended_at, area, category, summary,
           confidence real, evidence jsonb,          -- titles, process names, vision judgment text
           supersedes_id NULL REFERENCES detections,
           status detected|confirmed|disputed|superseded,   -- cached current state
           created_by_kind, created_by_id)
detection_events(id, tenant_id, detection_id, at, actor_kind, actor_id,
                 from_status, to_status, reason text)
```
- **Enforcement (implemented, migration 0005):**
  - `thelife_app` may INSERT detections but holds no UPDATE or DELETE on them, and no write privilege on `detection_events`.
  - Status moves only through `app.detection_transition(id, 'confirm'|'dispute', reason)` and `app.detection_claim(...)`. These are SECURITY DEFINER functions owned by the table owner. The owner is not BYPASSRLS, so the forced tenant policy still applies inside them.
  - A trigger rejects any change to content columns (everything except `status`, `superseded_by_id`, `updated_at`, `version`).
  - An AFTER UPDATE trigger writes the `detection_events` row, with the actor taken from the TenantTx settings and the reason from a transaction-local setting. History cannot be skipped.
  - New rows start as `detected`, or `confirmed` when `source = user`. `supersedes_id` can be set only by the claim function.
- **Transitions:** `confirm` (detected/disputed → confirmed), `dispute` (detected/confirmed → disputed, reason required) and `claim` (any but superseded). Repeating the current state, or acting on a superseded detection, answers 409.
- A user claim inserts a new `source=user` detection with `supersedes_id`, inheriting the time window unless one is given. The old one moves to `superseded` with `superseded_by_id` set. `GET /detections/{id}/history` returns the events and the whole supersede chain.
- Overlap policy **(decided)**: process and vision detections over the same window are both kept. Scoring uses the highest-precedence non-superseded detection per minute bucket, in the order `user > vision > process > external`.

### 6.6 Collector raw data (Stage 4, contract fixed now)
```
observations(id, tenant_id, device_id, client_seq bigint, captured_at,
             kind window|idle|process_snapshot|capture_meta,
             payload jsonb, UNIQUE(device_id, client_seq))
```
Retention: 30 days **(proposed)**, removed by a job. Detections keep only the extracted evidence.

### 6.7 Stage 2–3 tables (shape only)
```
conversations, messages(role, content, tool_calls jsonb, tool_call_id, tokens_in/out)
memories(tier episodic|profile, subject, content, sources jsonb, valid_from, superseded_by)
embeddings(owner_type, owner_id, chunk_ix, content_hash, model, model_version, embedding vector(1024))
agent_actions(audit: tool, args, result, decision, approval_id, conversation_id, principal)
approvals(action_id, status pending|approved|rejected|expired, decided_by, decided_at)
proposals(api_key_id, client_name, payload, status, adjudication_action_id)
scores(period day|week|month|year, period_start, breakdown jsonb, base, llm_adj_pct,
       final, inputs_hash, computed_at, is_current)        -- append-only versions
llm_usage(day, tokens_in, tokens_out, images)              -- budgets
jobs(kind, payload, run_at, attempts, status, locked_by, locked_at, last_error)
idempotency_keys(principal_id, key, request_hash, status_code, response, created_at)
```

## 7. API Conventions (`api/openapi.yaml`)

- Base path `/api/v1`, JSON only (multipart only for captures).
- **Errors:** RFC 9457 `application/problem+json` with a stable `type` URI, `title`, `detail`, `request_id`, and `errors[]` for validation failures.
- **Lists:** `?limit=` (default 50, max 200) and `?cursor=` (opaque, encodes `(sort_key, id)`); the response is `{ items, next_cursor }`. `?updated_since=` on every collection supports client sync.
- **Updates:** `PATCH` with JSON Merge Patch (RFC 7396). Responses carry `ETag: "<version>"`; `If-Match` is required on PATCH/DELETE, and a mismatch returns 412.
- **Idempotency:** `Idempotency-Key` header is accepted on all POSTs and required on collector ingest. The stored response is replayed for 24 h. Keys are 8–128 visible ASCII characters; a header that is present but empty is a 422, not an absent key.
- **Deletes:** soft (`deleted_at`). `?include_deleted=true` for admins and sync.
- **Database-raised errors:** SQL functions and triggers raise SQLSTATE class `TL` with a client-safe message: `TL422` maps to 422, `TL409` to 409 and `TL404` to 404. Constraint violations map generically: not-null to 422 on the column, check and FK violations to 422, unique to 409. All of SQLSTATE class 22 (data exceptions: a UTC offset beyond ±15:59, `\u0000` in text, out-of-range values) is 422, never 500.
- **Secrets are never replayed:** tools whose response carries a `token` (`devices.register`, `api_keys.issue`) refuse `Idempotency-Key` with 422, so plaintext tokens never sit in `idempotency_keys`. The contract does not offer the header on those operations (or on login).
- **Input normalisation:** integral JSON numbers (`8246.0`) become integers before validation, as JSON Schema already treats them.
- **Query parameters:** a parameter that is present but empty (`?to=`) is validated like any other value, so it is rejected rather than ignored.
- **405:** a known path called with a method it does not serve answers problem+json 405 with an `Allow` header. Methods Drogon's parser does not know (TRACE, CONNECT) get Drogon's bare 405 before application code runs.
- **Provenance:** every resource returns `created_by: {kind, id}`.
- **Versioning:** breaking changes go to `/api/v2`; additive changes stay in v1.

### 7.1 Endpoint catalog (Stage 1)

| Group | Endpoints | Auth / scope |
|---|---|---|
| Health | `GET /healthz`, `GET /readyz` (DB reachable, migrations current) | none |
| Auth | `POST /auth/login`, `POST /auth/logout`, `GET /me`, `PATCH /me` (tz, day_start_hour), `POST /me/password` | user |
| Devices | `POST /devices` → `{device, token}` once; `GET /devices`; `DELETE /devices/{id}` (revoke) | user |
| API keys | `POST /api-keys` `{name, scopes}` → token once; `GET`; `DELETE` | user |
| Goals / Plans | standard CRUD `/goals`, `/plans` | `planning:read/write` |
| Tasks | CRUD `/tasks`; `GET /tasks/{id}/occurrences?from&to`; `POST /tasks/{id}/occurrences/{date}:complete|:skip` | `tasks:*` |
| Calendar | CRUD `/events`; `GET /calendar?from&to` merges events, task occurrences, entertainment slots and skill sessions into one view | `calendar:*` |
| Activities | CRUD `/activities` | `activities:*` |
| Work | CRUD `/work/roles`, `/work/responsibilities`, `/work/projects`, `/work/deliverables` | `work:*` |
| Skills | CRUD `/skills/resources`, `/skills/resources/{id}/sections`, `/skills/sessions` | `skills:*` |
| Entertainment | CRUD `/entertainment/items`, `/entertainment/slots` | `entertainment:*` |
| Detections | `GET /detections?from&to&status&source`; `GET /detections/{id}` (with events and supersede chain); `POST /detections` (user/external); `POST /detections/{id}:confirm`; `POST /detections/{id}:dispute {reason}`; `POST /detections/{id}:claim {area, category, summary, started_at, ended_at}` | `detections:*` |

"Standard CRUD" means `GET list`, `POST`, `GET /{id}`, `PATCH /{id}`, `DELETE /{id}`.

### 7.2 Collector ingest (contract in Stage 0, implemented in Stage 4)

| Endpoint | Purpose |
|---|---|
| `GET /devices/self/config` | `enabled`, `capture_interval_s` (derived from the vision budget), `jitter_pct`, `capture_monitors`, `capture_max_px`, `idle_threshold_s`, `process_interval_s`. Returns `ETag`; the collector polls with `If-None-Match` (see `collector_lld.md` §6) |
| `POST /ingest/observations` | Batch of up to 500 `{client_seq, captured_at, kind, payload}`. Idempotent through `(device_id, client_seq)`; returns accepted and duplicate seqs |
| `POST /ingest/captures` | multipart: one image per monitor + JSON context (window, process snapshot, idle). Returns `202 {capture_id}`. The image is held **in memory only**, sent to the vision model by a worker and dropped; the resulting detections reference `capture_id` |
| `GET /devices/self/notifications?wait=55`, `POST /devices/self/notifications/{id}:ack` | Desktop notification long poll (`agent_lld.md` §8, `collector_lld.md` §7.5) |
| `POST /devices/self/token:rotate` | Issue a new device token; the old one stays valid for 5 min |
| `POST /devices/self/heartbeat` | `version, queue_depth, paused_until, x_ok, last_capture_at, last_error`; a missing heartbeat raises "collector silent" |

Device tokens carry only `ingest:write`, `devices:self` and `notifications:read`. **Decided (A):** all vision and LLM work lives in the backend. The collector holds no LLM credentials and does no interpretation; see §7.4.

### 7.4 Ingest processing pipeline (backend side, Stage 4)

The collector only reports raw facts. The backend turns them into meaning, so classification rules, title parsers and prompts can change without redeploying the collector.

```
POST /ingest/observations ──► observations table
                                   │  job: ingest.derive (debounced per device, ~1 min)
                                   ▼
             focus spans (window + idle merged) ──► classifier rules ──► detections(source=process)

POST /ingest/captures ──► bounded in-memory queue (no disk, default 8 slots; full → 503 + Retry-After)
                                   │  vision worker
                                   ▼
        images + context (focused window, recent focus spans, classified processes)
                ──► LLM vision call (structured output) ──► detections(source=vision, evidence.capture_id)
                ──► image buffers zeroed and freed in a finally path, success or failure
```

- **Classifier rules** (`ingest_rules`, versioned, per tenant plus global defaults) map `wm_class` / exe / title regex → `{app, category, area}`. Per-app title parsers (for example "file — project — VS Code" → project) live here, not in the collector.
- **Focus spans** are derived from window and idle observations: a span is one continuous focus on one window, with idle gaps cut out. They are the input for process detections and the context for vision.
- **Capture scheduling budget:** the backend computes `capture_interval_s` for `GET /devices/self/config` from the tenant's daily image budget, expected active hours (learned from the last 14 days, default 10 h), monitors per capture, and budget remaining today. The interval stretches as the budget runs low and goes to "off" when it runs out.
- If the vision call fails after retries, the capture is dropped and a `capture_meta` observation records the failure. Captures are never retried from disk.

**Implemented (Stage 4, increments 4a and 4b, 2026-11-16).**
- **Ingest** (`src/modules/ingest.cc`, migration 0015): payloads are validated per kind against `api/schemas/observations.json` (compiled in, shared with the collector tests), refused when `captured_at` is more than 10 min in the future, and stored **sealed** (`observations.payload`, fieldcrypt). One `INSERT … unnest … ON CONFLICT DO NOTHING` per batch; the response splits accepted and duplicate seqs. `Idempotency-Key` is required (contract flag `idempotency_required`). Observations are append-only for the app; `ops.observations_retention()` (housekeeping) removes them after 30 days.
- **Derivation** (`src/ingest/`): `ingest.derive` runs per device (coalesced, `THELIFE_INGEST_DERIVE_DELAY_S`, default 60) from the cursor in `ingest_state`. Focus spans end at a window change, a non-passive idle start (backdated to the last input), a lock, a session end or gap, or 5 min without any observation (collector off). A span still open is read again next time, unless it has run 30 min (then it is closed at the last observation and continues). Spans under 20 s are ignored; the same app/category/area across gaps ≤ 60 s merge into a block; blocks of 2 min or more become `detections(source=process)` with summary "App: subject" and evidence {app, top subjects with minutes, window class, exe}.
- **Rules:** built-in defaults in `config/ingest_rules.json` (compiled in; editors, terminals, office, chat, readers, games, video, and browsers by page title), the user's own in `/ingest/rules` (CRUD, also an agent tool in the detections group), checked first. Matching is case-insensitive; window class and exe are whole values with a trailing `*` prefix form, the title a substring; plain substrings instead of user regexes, so no pathological patterns. More match fields wins. Unmatched windows become category "unknown", area other, confidence 0.2; general browsing is 0.3 — below the 0.4 that scoring counts — until vision says more. Title parsers: vscode, jetbrains, browser, dash_app.
- **Config** (`src/ingest/config.cc`): `tenants.settings.vision` = {enabled (true), daily_captures (120, proposed), capture_monitors, capture_max_px}. Interval = expected active seconds / daily captures (10 h → 5 min), stretched to (seconds left today / captures left) when fewer are left; off when none are left or no vision-capable provider is available. Captures used today are counted from `llm_calls` (purpose `vision.analyze`). The response carries a content `ETag`; `If-None-Match` answers 304 (contract flag `conditional_get`).
- **Heartbeat** sets `devices.last_seen_at` (the notifier's "away" check) and `devices.status`. **Token rotation** issues a new token and sets the old one to expire in 5 min.
- **Captures** (`src/vision/captures.cc`): the REST adapter parses multipart in memory; file parts become `SecureBuffer` attachments (zeroed on destruction), never part of the arguments, the idempotency hash or any log. JPEG magic and the 8 MiB part limit are checked. 409 `config-stale` when capture is off, 503 + `Retry-After` when all `THELIFE_VISION_QUEUE` (8) slots — queued plus in flight — are taken. One worker sends the images (data URIs) and a context (focused window, top processes by CPU, detections of the last 30 min, calendar blocks now) to the vision model with `jobs/vision_analyze` (structured: area, category, summary, confidence, apps) and writes one `detections(source=vision)` covering the capture interval up to the capture time (≤ 15 min), evidence {capture_id, monitors, apps, focused window}. Failures are logged by class and dropped, no retry. The prompt forbids transcribing on-screen text.
- **No body ever reaches the disk:** Drogon spills request bodies above `client_max_memory_body_size` (default 64 KiB) to files under the upload path. The backend sets the memory limit equal to the body limit (48 MiB) and moves the upload path out of the working directory (`THELIFE_UPLOAD_DIR`). A test watches the upload tree with inotify during a 500 KiB capture; with the old default it saw the body written twice.
- **Best effort on zeroing:** our copies (attachments, base64) are zeroed; the framework's request buffer and the provider's HTTP body are freed, not zeroed.

### 7.3 Admin (no HTTP surface in Stage 1)
`thelife-admin tenant create --name`, `thelife-admin user create --tenant --email` (prints a one-time password), `thelife-admin tenant kill-switch on|off`. It connects as `thelife_admin`. An HTTP admin API waits until a management UI is in scope.

## 8. Tool Registry (the "define once" seam)

```cpp
enum class Effect { Read, Create, Update, Delete };

struct ToolDef {
  std::string name;                 // "tasks.create"  (dot form; REST/MCP map from it)
  std::string description;          // shown to LLM and in MCP
  Json::Value input_schema;         // JSON Schema 2020-12
  Json::Value output_schema;
  Effect effect;
  std::vector<std::string> scopes;  // required for api_key/device principals
  uint8_t exposure;                 // REST | AGENT | MCP bitmask
  std::function<drogon::Task<Json::Value>(ToolCtx&, const Json::Value&)> handler;
  // optional: resolves the target entity so policy can inspect provenance
  std::function<drogon::Task<std::optional<Target>>(ToolCtx&, const Json::Value&)> target;
};
```

`ToolRegistry::invoke(name, ctx, args)`:
1. Validate `args` against `input_schema` (valijson) → 422 on failure.
2. `Policy::decide(principal, tool, target)` → `Allow | RequireApproval | Propose | Deny`:
   - `user` principal → Allow (the user owns their data; scope checks still apply to API keys).
   - `agent` principal → the 5.3 table: Read and Create are allowed; Update is allowed if `target.created_by_kind == agent && target.origin != user_stated` (or it is an `undo` within 24 h of a receipt), otherwise RequireApproval; Delete → RequireApproval; the kill switch turns every write into Deny.
   - `api_key` with MCP origin → Read is allowed; writes → Propose (stored in `proposals`).
3. Allow → run handler in a TenantTx and write an `agent_actions` row for non-user principals (Stage 1 records only principal and tool; the full audit arrives in Stage 2).
4. RequireApproval / Propose → persist the action and return `202 {approval_id | proposal_id}`.

REST controllers are thin: map path/query/body to `args`, call `invoke`, map the result to an HTTP response. OpenAPI, the registry manifest and the agent tool definitions are all generated from one source (`api/common.yaml` + `api/resources/*.yaml` → `api/generated/*` via `api/tools/contractgen.py`, checked in CI). Details: `docs/tool_registry.md`.

## 9. Security Details

- Passwords: argon2id (libsodium `crypto_pwhash`, moderate limits). Login is rate-limited per email and per IP (in-memory token bucket while single-process).
- User session tokens: 30-day sliding expiry **(proposed)**; `last_used_at` is updated at most once per minute per token.
- **Web UI session (Stage 5, `frontend_lld.md` §2).**
  - `POST /auth/web-session` sets the session token as the cookie `tl_session` (HttpOnly, Secure, SameSite=Strict, Path=/api). The token never appears in the body.
  - The authenticator falls back to that cookie only when there is no `Authorization` header, and accepts only `tlu_` user sessions from it.
  - Cookie-authenticated POST, PATCH, PUT and DELETE requests need `X-TheLife-CSRF: 1` (otherwise 403 `csrf-required`).
  - `THELIFE_WEB_ROOT` serves the built UI with an SPA fallback and a strict CSP (`src/http/web_root.cc`).
  - Drogon's default static file serving from the working directory is turned off (`setFileTypes({})`). Before this, it served files such as `.txt` or `.svg` from the working directory.
  - Unknown `/api/*` paths answer with a problem+json 404.
- **Envelope encryption: deferred to Stage 2 (decision C).** Stage 1 holds no encrypted columns. Stage 2 adds a per-tenant DEK in `tenant_keys`, wrapped by a KEK from a file or env var. Encrypted fields become `bytea` and opaque to SQL (no `ILIKE`, no FTS), so the field list is chosen in Stage 2. Free-text columns are kept in their own column (never folded into jsonb) so they can be converted in place later.
- **Implemented (2026-11-13, migration 0014, `src/db/field_crypt.*`).**
  - **What is encrypted** (decided 2026-11-13: what is sensitive and never searched): free-text notes and descriptions (`goals.description`, `plans.body`, `work_roles/responsibilities/projects/deliverables.description`, `tasks.notes`, `calendar_events.notes`, `activities.notes`, `skill_sessions.notes`, `entertainment_slots.notes`), detection `summary` and `evidence`, and (Stage 4) collector observations from the start. Chat messages, memory facts, day summaries, digests and all titles stay plaintext so keyword search keeps working.
  - **Threat model:** protects dumps, backups and direct database access. The KEK sits with the app, so a compromised app reads everything.
  - **Keys:** `THELIFE_KEK_FILE` (preferred) or `THELIFE_KEK`, 32 bytes as base64 or hex; the backend refuses to start without one when a database is configured. Each tenant's DEK is random, created on first use in its own committed transaction, and stored wrapped (XChaCha20-Poly1305, AD `tenant_keys|<tenant>`) with `kek_id` (a sha256 prefix of the KEK) so a wrong KEK fails with a clear message. The app may insert and read `tenant_keys`, never change it; rotation is a future operator task.
  - **Values:** `0x01 | nonce(24) | ciphertext` of the value's JSON text, AD `<tenant>|<table>.<column>`: a value copied to another tenant or column does not open. Rows are not bound (a value moved between two rows of the same column still opens). The old length checks became an `octet_length ≤ 65536` cap; the contract still enforces the character limits.
  - **Where:** the generic CRUD layer seals on write and opens on read, so the API is unchanged. Other readers open explicitly: detection claim and history (`app.detection_claim` now takes the sealed summary), `history.search` (activity notes no longer match keywords, only titles; the hit text shows the opened notes) and embeddings (activity text = title + opened notes; the change hash covers the ciphertext). The `detection.disputed` outbox event carries the category instead of the summary.
  - **Existing data:** the migration rewrote each value as `0x00 | JSON text` (still readable) and queued one `fieldcrypt.reseal` job per tenant, which seals them (detections through `app.detection_reseal`, the only exception to append-only). The down migration works while no value is sealed.
  - **Dev:** `scripts/demo.sh` creates `build/dev-kek` on first use; tests use a fixed test KEK (`tests/support/server_process.h`).
- Analytics: a `usage_events(kind, count, duration_ms)` table with no free text. Content never goes to logs; titles and notes are logged as lengths only.

## 10. Background Jobs

A Postgres-backed queue (`jobs` table). The in-process worker coroutine claims work through `jobs.claim()` (`FOR UPDATE SKIP LOCKED`), sets the tenant context from the job row and runs the handler. Exponential backoff, `max_attempts`, dead status.
Stage 1 jobs: `occurrences.mark_missed` (daily per tenant), `observations.retention`, `idempotency.gc`.

**Implemented (Stage 2, increment 3):** `ops.jobs` (deployment state in the `ops` schema, like `provider_state`;
payloads hold ids and dates only) with `FOR UPDATE SKIP LOCKED` claims, a partial unique index on
`(kind, dedupe_key)` for queued/running jobs (bursts coalesce), exponential backoff (1 min doubling, 6 h cap) up to
`max_attempts`, then `dead`. A handler throws `jobs::Defer` to wait (assistant offline, rate limit) without
spending an attempt. Housekeeping every minute calls SECURITY DEFINER functions owned by `thelife_definer`:
`ops.enqueue_day_closes()` (missed-occurrence marking now runs inside `day.close`), `ops.fail_stale_turns()` (turns
left running by a crash) and `ops.gc_idempotency_keys()`, and requeues jobs whose worker vanished. Jobs are
enqueued inside the tenant transaction that causes them. Knobs: `THELIFE_JOBS=off`, `THELIFE_JOBS_CONCURRENCY`,
`THELIFE_JOBS_POLL_MS`, `THELIFE_JOBS_HOUSEKEEPING=off`, `THELIFE_JOBS_TENANT` (claim one tenant's jobs only;
tests on the shared database use it, and test servers default to `THELIFE_JOBS=off`). `observations.retention`
arrives with Stage 4.
Stage 2 job kinds: `day.close`, `memory.compress_day`, `memory.rollup`, `embeddings.sync`,
`scores.close_day`, `scores.close_period`, `proactive.tick`, `notify.telegram` (Telegram delivery and the 3-minute fallback,
`agent_lld.md` §8) and `digest.refresh` (cross-channel digest, §7.3).
Later: `vision.analyze`, `memory.summarize`, `embeddings.upsert`, `scores.recompute` (triggered on detection or activity change, debounced per tenant-day).

## 11. Configuration and Observability

- `config/*.json` for Drogon (listeners, threads, DB pool size); secrets only from env (`THELIFE_DB_URL`, `THELIFE_KEK_FILE`, `THELIFE_LLM_*`, `OLLAMA_API_KEY`, `THELIFE_TELEGRAM_BOT_TOKEN`). The bot token is part of every Bot API URL, so request URLs to Telegram are never logged.
- Structured logs (one line, key=value) with request id, principal kind and tenant id hash.
- `GET /metrics` (Prometheus text) **(proposed)**: request counts and latency, DB pool, job queue depth, LLM tokens.

## 12. Testing Plan (Stage 0–1)

| Suite | What | How |
|---|---|---|
| unit | policy decisions, RRULE expansion, day-boundary math, cursor encoding, problem+json mapping | GoogleTest, no DB |
| integration | repositories and services against real Postgres | each test creates its own tenant, so tests never interfere and need no truncation |
| isolation | catalog checks + generated per-table leak tests (§4.4) | runs on every PR |
| api | every endpoint: happy path, validation 422, auth 401, scope 403, If-Match 412, cross-tenant 404 | server binary on a random port, Drogon `HttpClient` from GTest |
| contract (implemented) | every user-reachable Stage 1 operation against `openapi.yaml`: no 5xx, documented status codes, content types, headers and response schemas, invalid input rejected, missing If-Match → 428. Schema-valid requests may still get 409/412/422 for rules a schema cannot state (`schemathesis.toml` lists them); `run.sh --strict` shows them for review | **schemathesis 4.33** in the backend CI job via `backend/tests/contract/run.sh --with-login`, then the backend log must hold no unhandled exception |
| detection invariants | UPDATE/DELETE blocked, transitions recorded, claim → superseded | isolation (as `thelife_app`) + api |
| manifest matrix (implemented) | every Stage 1 REST tool: device token → 403, API key without scope → 403, update/delete without If-Match → 428, target reads with an unknown id → 404 | `api/contract_test.cc`, driven by the embedded `tools.json` |
| resource lifecycle (implemented) | every CRUD resource: create/get/list/patch/delete, ETag/412/428, other tenant 404 and empty list. A new resource fails CI until it has a case | `api/resources_test.cc` |
| proactivity (implemented, Stage 2) | reminders from templates (once, repeating, cancelled when the task is done), follow-ups as proactive turns, gate spacing and urgent bypass, suppressed messages, no_message without another model call, routines once a day and skip_once without approval, silent dispute review with the detections tools, missed-task triggers coalesced, entertainment warning once per session, deferred replies after the reset, notification acks, turn context on a day without user messages | `api/proactive_test.cc` (scheduling function called by the test, tenant-scoped worker, fake-llm) |
| scoring (implemented, Stage 2) | golden days (workday, weekend, leave, unobserved, self-reported work, replanned item, overlap precedence and preemption, linked blocks, bonus cap, consistency) and properties (bounds, monotonic, non-applicable areas, hash idempotence); end to end: day score with adjustment, idempotent and late re-score reusing the adjustment, dispute re-run, provisional in manual mode, unobserved and leave days, settings, today's progress, week score | `unit/scoring_test.cc`; `api/scoring_test.cc` with a tenant-scoped worker and fake-llm |
| embeddings and web search (implemented, Stage 2) | sync after turns, re-embedding of changed items, hybrid history search (keyword + vector, reciprocal rank fusion, `match` per hit), semantic fact recall, related-from-earlier context, keyword fallback and deferred sync without the embedder; web search/fetch results cleaned and marked untrusted, writes after web content held for approval, per-turn and daily limits, unconfigured and rejected-key errors | `api/embeddings_test.cc` with `docker/fake-embedder`; `api/web_test.cc` with fake-llm's web endpoints; `embedder/embedder_test.cc` on the GPU when the model files exist |
| memory and jobs (implemented, Stage 2) | remember/recall/update/forget with provenance and approvals, mentioned-entity retrieval, history search, day close (summary, facts active vs proposed, conversation closed, routine missed, exactly once), deferral while the model is offline, week rollup; named policies in unit tests | `api/memory_test.cc` with a tenant-scoped worker against `docker/fake-llm`; `unit/tools_test.cc` |
| agent (implemented, Stage 2) | chat capture provenance (`user_stated`, `source_message_id`) and audit, tool groups, approvals (held → approve runs pinned to the version, stale on change, reject, 72 h expiry), kill switch, `user_reported_progress` and undo, weekly limit → deterministic replies and commands, step limit, model and argument errors, idempotent sends, SSE events, turn merging, desktop notification long poll and ack | `api/agent_test.cc`: the real binary against `docker/fake-llm` with scripted conversations |
| LLM gateway (implemented) | error classification against the recorded Stage 0 bodies, response and SSE parsing (split anywhere, leaked `</think>` retracted), provider state machine on a fake clock (transient backoff, weekly exhaustion → manual → probe → healthy, restart persistence, auth → disabled, rate limits), structured output (tool-as-output → repair → schema-in-prompt) | `unit/llm_test.cc`; `api/llm_test.cc` over libcurl against `docker/fake-llm` plus `ops.provider_state` / `llm_calls` / `llm_usage` in Postgres; `live/llm_live_test.cc` against the real endpoint when `THELIFE_LIVE_LLM_URL` is set |
| demo UI (implemented) | contract-generated pages render for every resource; forms send contract bodies (local time → RFC 3339, merge patch with If-Match, only changed fields); tokens never reach the browser; end-to-end flows against the built backend | `demo-ui/tests` (pytest, fake backend); `test_e2e_live.py` in the backend CI job |

Cross-tenant reads return **404, not 403**, so ids from other tenants cannot be probed.

## 13. Decisions (review round 1, 2026-11-04)

| # | Decision | Outcome |
|---|---|---|
| A | Where the vision call runs | **Backend only.** The collector is a headless capture-and-upload agent. Interpretation (title parsing, process classification, vision, detection derivation) all happens server-side (§7.4) |
| B | Package manager | vcpkg (manifest mode) |
| C | Envelope encryption | Deferred to Stage 2; implemented 2026-11-13 for notes, descriptions and detection text (§9) |
| D | Migrations tool | dbmate |
| E | Detection overlap precedence | `user > vision > process > external` per minute bucket |
| F | Day boundary | per-user `day_start_hour`, default 4 |
| G | Entertainment in scoring | tracked, weight 0 |
| H | Contract testing | schemathesis in CI |

## 14. Spec Gaps Found During Review (feed back into `prelim_spec.md`)

1. Detection is "append-only", yet its status changes. Resolved here as immutable content + event log + cached status (§6.5).
2. The spec doesn't say where the vision call runs. Resolved: backend (decision A).
3. Timezone and day-boundary rules for daily scoring were not defined (§5).
4. No recurrence representation was given. RRULE was chosen (§6.3).
5. The collector offline queue needs idempotent ingest (§7.2 `client_seq`, `Idempotency-Key`).
6. The device registration and token issuance flow was not defined (§7.1 Devices).
7. "Edit agent-created items" requires provenance on every row (§5 `created_by_kind`).
8. Login and token lookup need a defined cross-tenant path under RLS (§4.3).
9. Encryption conflicts with search and FTS, which limits the choice of fields. Deferred to Stage 2 (decision C).
10. Embedding inference in-process: a CUDA fault would take down the API. Consider applying the Stage 6 "separate process" rule to bge-m3 as well (decide in Stage 2).
