# TheLife v2: Backend Low-Level Design

Status: Draft 2. Builds on `prelim_spec.md`. Decisions from review round 1 are recorded in §13. Items still marked **(proposed)** await confirmation.
Scope: Stage 0 and Stage 1 in detail, with the Stage 2–4 seams (tool registry, jobs, collector ingest) fixed now so later stages do not reshape the core.

---

## 1. Repository Layout (monorepo) **(proposed)**

```
TheLife_v2/
  api/
    openapi.yaml              # source of truth for the public API (OpenAPI 3.1)
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
| Recurrence | libical for RFC 5545 RRULE expansion |
| Tests | GoogleTest + real PostgreSQL 17 with pgvector from docker compose |
| Migrations | **dbmate (decided)**: plain SQL, one static binary, no ORM coupling |
| CI | GitHub Actions: build, unit, integration (service container), isolation, contract tests |

Async model: Drogon C++20 coroutines (`drogon::Task<>`, `co_await` on DB and HTTP clients) throughout. No blocking calls on event-loop threads. The future embedding call runs on a dedicated thread pool.

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

### 6.2 Planning
```
goals(…common, title, description, area, horizon year|quarter|month|week,
      parent_id, target jsonb, starts_on, due_on, status)
plans(…common, goal_id NULL, title, body, period_start, period_end, status)
```
`area` enum: `work | skill | fitness | personal | entertainment | other`. It lines up with the scoring weights (5.7); entertainment is tracked but weighted 0 **(decided)**.

### 6.3 Tasks and calendar
```
tasks(…common, title, notes, area, goal_id, project_id, priority 0-3,
      estimate_min, due_at, status open|done|cancelled,
      rrule text NULL, rrule_dtstart timestamptz NULL)       -- recurring if rrule set
task_occurrences(…common, task_id, occurs_on date, status todo|done|skipped|missed,
                 completed_at, UNIQUE(task_id, occurs_on))
calendar_events(…common, title, notes, starts_at, ends_at, all_day,
                rrule, location, area, link_type, link_id)
```
Recurrence is expanded on read for ranges. Occurrence rows are materialized lazily, either when the user or agent acts on one or when the nightly job marks `missed`, so the table only holds occurrences that have a state.

### 6.4 Activities, work, skills, entertainment
```
activities(…common, area, category, title, started_at, ended_at,
           source manual|detection|agent, detection_id NULL, notes)
work_roles / responsibilities(role_id) / projects(role_id) / deliverables(project_id, due_at, status)
skill_resources(…common, kind book|course, title, author, total_pages, total_duration_s, status)
course_sections(…common, resource_id, ordinal, title, duration_s)
skill_sessions(…common, resource_id, started_at, ended_at,
               page_from, page_to,                 -- books
               section_id, pos_from_s, pos_to_s,    -- courses
               notes, source)
entertainment_items(…common, kind game|movie|series, title, meta jsonb, status)
entertainment_slots(…common, item_id, starts_at, ends_at, consumed bool)
```

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
- A trigger rejects `UPDATE` on any column except `status`, and only allows `status` to change together with a matching `detection_events` insert (done in one SQL function, `detections_transition()`). `DELETE` is revoked from `thelife_app`.
- A user claim inserts a new `source=user` detection with `supersedes_id`, and the old one moves to `superseded`.
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
- **Idempotency:** `Idempotency-Key` header is accepted on all POSTs and required on collector ingest. The stored response is replayed for 24 h.
- **Deletes:** soft (`deleted_at`). `?include_deleted=true` for admins and sync.
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

REST controllers are thin: map path/query/body to `args`, call `invoke`, map the result to an HTTP response. OpenAPI request bodies and tool `input_schema` come from the same JSON Schema files (`api/schemas/*.json`, referenced by both the OpenAPI file and registry registration). That is how "one tool registry" stays honest.

## 9. Security Details

- Passwords: argon2id (libsodium `crypto_pwhash`, moderate limits). Login is rate-limited per email and per IP (in-memory token bucket while single-process).
- User session tokens: 30-day sliding expiry **(proposed)**; `last_used_at` is updated at most once per minute per token.
- **Envelope encryption: deferred to Stage 2 (decision C).** Stage 1 holds no encrypted columns. Stage 2 adds a per-tenant DEK in `tenant_keys`, wrapped by a KEK from a file or env var. Encrypted fields become `bytea` and opaque to SQL (no `ILIKE`, no FTS), so the field list is chosen in Stage 2. Free-text columns are kept in their own column (never folded into jsonb) so they can be converted in place later.
- Analytics: a `usage_events(kind, count, duration_ms)` table with no free text. Content never goes to logs; titles and notes are logged as lengths only.

## 10. Background Jobs

A Postgres-backed queue (`jobs` table). The in-process worker coroutine claims work through `jobs.claim()` (`FOR UPDATE SKIP LOCKED`), sets the tenant context from the job row and runs the handler. Exponential backoff, `max_attempts`, dead status.
Stage 1 jobs: `occurrences.mark_missed` (daily per tenant), `observations.retention`, `idempotency.gc`.
Later: `vision.analyze`, `memory.summarize`, `embeddings.upsert`, `scores.recompute` (triggered on detection or activity change, debounced per tenant-day).

## 11. Configuration and Observability

- `config/*.json` for Drogon (listeners, threads, DB pool size); secrets only from env (`THELIFE_DB_URL`, `THELIFE_KEK_FILE`, `THELIFE_LLM_*`).
- Structured logs (one line, key=value) with request id, principal kind and tenant id hash.
- `GET /metrics` (Prometheus text) **(proposed)**: request counts and latency, DB pool, job queue depth, LLM tokens.

## 12. Testing Plan (Stage 0–1)

| Suite | What | How |
|---|---|---|
| unit | policy decisions, RRULE expansion, day-boundary math, cursor encoding, problem+json mapping | GoogleTest, no DB |
| integration | repositories and services against real Postgres | each test creates its own tenant, so tests never interfere and need no truncation |
| isolation | catalog checks + generated per-table leak tests (§4.4) | runs on every PR |
| api | every endpoint: happy path, validation 422, auth 401, scope 403, If-Match 412, cross-tenant 404 | server binary on a random port, Drogon `HttpClient` from GTest |
| contract | responses conform to `openapi.yaml` | **schemathesis (decided)** against the running service in CI |
| detection invariants | UPDATE/DELETE blocked, transitions recorded, claim → superseded | integration |

Cross-tenant reads return **404, not 403**, so ids from other tenants cannot be probed.

## 13. Decisions (review round 1, 2026-11-04)

| # | Decision | Outcome |
|---|---|---|
| A | Where the vision call runs | **Backend only.** The collector is a headless capture-and-upload agent. Interpretation (title parsing, process classification, vision, detection derivation) all happens server-side (§7.4) |
| B | Package manager | vcpkg (manifest mode) |
| C | Envelope encryption | Deferred to Stage 2 |
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
