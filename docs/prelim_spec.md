# TheLife v2: Planning Spec

Status: Draft 1 (planning). Stages are sequential and paced by availability, so there are no calendar timelines.
Items marked **(proposed)** are suggestions awaiting confirmation. Everything else was decided in discussion.

---

## 1. Purpose and Scope

v1 worked, but it relied on manual data entry, so it was under-used. v2 keeps the v1 fundamentals and makes the system:

- **Automated.** The system observes what the user does and records it as detections.
- **Agent-driven.** An agent with tools can read and change data and guide the user through chat.
- **Low-overhead.** The user mainly supplies plans and goals and acknowledges or disputes detections.
- **Open.** Stable API contracts and an MCP interface let external apps and agents connect.

**v1 fundamentals carried over:** tasks (recurring and ad hoc), calendar and schedules, activities, work management, skill management (books and courses), entertainment planning, goals and plans, and daily/weekly/monthly/yearly scoring.

**Starting point:** v1 data is not migrated. v2 starts fresh.

## 2. Locked Decisions

| Area | Decision |
|---|---|
| Backend | C++20, Drogon framework |
| Collector | C++20, separate process |
| Frontend | React + TypeScript, built later with coding agents from UI specs |
| Interim UI | Flask minimal demo UI used to test backend and collector in parallel |
| Database | PostgreSQL + pgvector |
| LLM interface | OpenAI-compatible HTTP/REST to Ollama cloud model `glm-5.3-flash:cloud` for both the main agent and the vision model |
| Embeddings | bge-m3 run locally on the CUDA GPU (about 1 GB of a 12 GB GPU), 1024 dimensions |
| Custom inference | libtorch, ONNX Runtime (CPU and GPU) and TensorRT (Volta and Ampere); deferred, see Stage 6 |
| Topology | Same machine first, backend and collector as separate processes talking only through the public API; remote server later |
| Platform | Ubuntu 24.04, X11 only (no Wayland) |
| Screenshots | Never stored. Only the vision agent's judgments and markings are kept |
| Privacy | Raw screenshots go to the cloud vision model without redaction. A local small vision model (for example lfm or qwen-vl series) is planned for when other users are added |
| Tenancy | Full multi-tenant from day one |
| Accounts | Created by an admin only |
| Detections and scores | Unconfirmed detections count toward the score. A user dispute triggers re-evaluation by the main agent |
| Agent autonomy | High. Approval is required only for gated actions (see 5.3) |
| Tests | Every backend and collector operation has functional tests; a stage is not done until its tests pass and earlier stages still pass |

## 3. Design Principles

1. **One tool registry.** Each capability is defined once and exposed to the main agent, the internal API and MCP. No separate implementations.
2. **API first.** The OpenAPI contract is written before implementation. The collector, the Flask demo UI, the React frontend and external apps are all ordinary API clients.
3. **Append-only evidence.** Detections and agent actions are recorded, not overwritten. Corrections are new records that reference the old ones.
4. **Scalable by modules.** New features arrive as new domain modules and tools without changing the core.
5. **Test as you build.** Tests are part of each stage's definition of done, not a later phase.
6. **Isolation by default.** Cross-tenant access is blocked at the database level, not only in application code.

## 4. Architecture Overview

```
                 +------------------+        +----------------------+
 External agents | MCP interface    |        | Flask demo UI (S1-4) |
 (Claude Code,   | read / add       |        | React frontend (S5)  |
  Hermes)        | context / propose|        +----------+-----------+
                 +--------+---------+                   |
                          |                             |
                 +--------v-----------------------------v-----------+
                 |              Drogon backend (C++20)              |
                 |  Public REST API  |  Tool registry               |
                 |  Auth / tenancy   |  Agent harness + memory      |
                 |  Domain modules   |  LLMOps client               |
                 |  Scoring engine   |  Embedding service           |
                 +--------+----------+----------+-------------------+
                          |                     |
                 +--------v-------+     +-------v--------+
                 | PostgreSQL     |     | LLM endpoint   |
                 | + pgvector     |     | (Ollama cloud) |
                 +----------------+     +----------------+
                          ^
                          | public API only (device token)
                 +--------+---------------------------+
                 | Collector (C++20, X11)             |
                 | screenshots, active window, idle,  |
                 | process snapshot, offline queue    |
                 +------------------------------------+
```

Components:

- **Backend:** the single source of truth. Hosts the API, tool registry, agent harness, memory, scoring and embedding service.
- **Collector:** runs in the user's desktop session. It never talks to the database. It sends observations to the backend through the public API using a device token.
- **Embedding service:** bge-m3 through ONNX Runtime (CUDA execution provider) **(proposed)**, behind an internal interface so the runtime can change.
- **Interim and final UIs:** plain API clients. They hold no business logic.

## 5. Cross-Cutting Specifications

### 5.1 Multi-tenancy and security

- Every table carries `tenant_id`. PostgreSQL row-level security enforces it, including the vector tables.
- The application sets the tenant on each database session. Queries without a tenant context return nothing.
- Per-tenant encryption keys protect sensitive fields **(proposed: envelope encryption)**. The server must decrypt data to run the agent, so this protects against database leaks and casual admin access, not against the running service itself.
- Developer-side analytics collect usage patterns only (counts, timings), never content such as task or activity text.
- Auth types: user tokens, device tokens (collector) and scoped API keys (external apps and MCP).
- Accounts are created by an admin. There is no self-signup.

### 5.2 Domain model (core entities)

Tasks (recurring and ad hoc), schedule and calendar events, activities, goals and plans, work (roles, responsibilities, deliverables, projects), skills (books with page ranges per session; video courses with sections and timeline), entertainment (games, movies, series, pre-scheduled), scores, conversations, memories, agent actions, proposals, devices.

**Detection** (new in v2):

- Source: `vision`, `process`, `user` or `external`.
- Time range, category (which section it maps to), summary, confidence and evidence references (process list, window titles, vision judgment text; no images).
- Status: `detected`, `confirmed`, `disputed` or `superseded`.
- Score impact: computed while `detected` or `confirmed`, re-evaluated on `disputed`.
- Records are append-only. A user claim creates a new linked record.

### 5.3 Agent permission policy

| Action | Policy |
|---|---|
| Read any of the user's data | Automatic |
| Create tasks, activities, schedule entries, detections | Automatic, logged |
| Edit agent-created items | Automatic, logged |
| Edit user-entered plans or goals | **Approval required** |
| Delete anything | **Approval required** |
| Bulk reschedule (threshold to be set) | **Approval required** **(proposed)** |
| Changes proposed by external agents (MCP) | Held as proposals; the main agent adjudicates (see 5.6) |

Every agent action is written to an audit log with tool, arguments, result and the conversation that triggered it. A per-tenant kill switch disables agent writes.

### 5.4 Memory (hybrid multi-tier)

1. **Working memory:** the last 2 conversations, verbatim, in the agent's context.
2. **Episodic memory:** older conversations stored as summaries that become shorter as they age. Summarization runs as a background job.
3. **Semantic memory:** activities, logs and notes embedded with bge-m3 (1024-dim) in pgvector, retrieved by similarity for the agent.
4. **Profile memory:** structured goals, preferences and long-term facts the agent maintains, with changes to user-entered items subject to 5.3.

Retrieval window for history-aware answers is 3 months, as in the v1 vision. Re-embedding is required if the embedding model ever changes, so the model name and version are stored with each vector.

### 5.5 LLM interface (LLMOps)

- OpenAI-compatible client: chat completions, streaming, tool calls, structured output, vision input.
- Per-tenant token and image budgets. The collector's capture frequency is derived from the vision budget.
- Retries with backoff, timeouts and request logging.
- A fake OpenAI-compatible server with recorded responses is used for tests.
- Stage 0 verifies that `glm-5.3-flash:cloud` supports vision, tool calling and structured output. The design adapts to the results.

### 5.6 External interfaces

- **Internal and public REST API:** OpenAPI-first, versioned.
- **MCP server:** external agents (Claude Code, Hermes) can read data, add context, and **propose** changes. They cannot modify directly. Proposals enter a queue and the main agent accepts, edits or rejects them under 5.3.
- **Webhooks** for outbound events **(proposed)**.
- All external access uses scoped API keys.

### 5.7 Scoring

- Daily score out of 100, with cumulative weekly, monthly and yearly scores.
- Weights from v1 settings are the starting point: work 0.30, skill 0.25, fitness 0.15, personal 0.15, consistency 0.15.
- The LLM adjustment of up to plus or minus 30 percent from v1 is retained for alignment with long-term goals.
- Detections feed the score. A disputed detection is re-evaluated by the main agent, which also tracks whether disputes follow a pattern that suggests gaming and flags it.

## 6. Stages

Each stage has deliverables and an exit gate. There are no timelines; stages run sequentially.

### Stage 0: Contracts and foundations
**Deliverables**
- OpenAPI contract draft for the public API.
- Tool-registry spec and the permission policy table (5.3).
- Domain and database schema design.
- Repository layout, CMake build, package manager choice **(proposed: vcpkg or Conan)**, CI and test harness.
- Spike: confirm `glm-5.3-flash:cloud` capabilities (vision, tools, structured output) and bge-m3 on GPU through ONNX Runtime.

**Exit gate:** contracts reviewed and agreed; empty Drogon service builds and passes a health-check test in CI.

### Stage 1: Backend core
**Deliverables**
- Drogon service with migrations, row-level security, auth (user tokens, device tokens, API keys) and admin account creation.
- CRUD APIs for the v1 domain plus the detection entity (5.2).
- Flask demo UI, first increment: login and basic views.

**Exit gate:** every endpoint has functional tests; cross-tenant leak tests pass for every table.

### Stage 2: LLMOps and agent harness
**Deliverables**
- LLM client (5.5), tool registry wired to the domain modules, agent loop.
- Approval gating, approval queue and audit log (5.3).
- Memory tiers (5.4) and the local embedding service.
- Chat API with streaming.
- Scoring engine and the dispute-handling flow (5.7).
- Flask demo UI: chat, approvals, scores.

**Exit gate:** agent flows pass tests against the fake LLM server; live smoke tests pass; gated actions never execute without approval.

### Stage 3: External interfaces
**Deliverables**
- MCP server with read, add-context and propose tools.
- Proposal queue and adjudication flow.
- Scoped keys, API versioning, webhooks.

**Exit gate:** an external MCP client can read data and submit proposals, and cannot write directly (tested).

### Stage 4: Collector (X11)
**Deliverables**
- Per-monitor screenshot capture, active window and title, idle time, and a refined process snapshot.
- Scheduler with capture frequency derived from the vision budget.
- Offline queue and retry; device-token authentication.
- Vision pipeline: screenshot plus concatenated process context goes to the vision model and returns detections. The image is discarded after the call.
- Flask demo UI: detection timeline and claim or dispute actions.

**Exit gate:** collector tests pass under Xvfb with synthetic windows; end-to-end test from capture to detection record passes with the fake LLM server.

**Met (2026-11-16):** `ctest -L collector` (unit, Xvfb system and fuzz-corpus tests) and the two collector e2e tests against the real backend with the fake LLM server: a capture becomes `detections(source=vision)`, and 150 s of focus on a synthetic window becomes `detections(source=process)`. The demo UI timeline (`/timeline`) confirms, disputes and corrects detections; its live test runs against the real backend.

### Stage 5: Frontend (React TS)
**Deliverables**
- UI specs per screen, then iterative builds with coding agents.
- OLED dark theme with cyan, emerald green and dark pink accents.
- Screens: login, dashboard with day, week and month calendar, chat, detection review, approvals, and the v1 sections.
- Verification of each screen with Claude in Chrome.

**Exit gate:** the frontend replaces the Flask demo UI with no loss of function.

**Design:** `docs/frontend_lld.md` (screen specs, increments 5a–5e). Started 2026-11-16.

**Met (2026-11-17):** the parity gate (`frontend/scripts/parity.mjs`, in CI) finds every user operation of the contract
called by the web UI (160/160), and each hand-written demo page maps to a screen with a Playwright test against the
real backend (desktop and phone). The Flask demo UI was retired: moved to `spikes/demo-ui` for reference, no longer
started by `scripts/demo.sh` or run in CI.

### Stage 6 (deferred): Custom inference
- Scheduling, measurement and analytics models.
- ONNX Runtime first (with the TensorRT execution provider), then libtorch where needed.
- Confirm the TensorRT version that supports both Volta and Ampere before starting.
- Runs as a separate service or process behind a stable interface so GPU faults do not take down the API **(proposed)**.

## 7. Testing Strategy

- **Framework:** GoogleTest **(proposed)**, with integration tests against a real PostgreSQL.
- **Isolation tests:** every table gets a cross-tenant leak test.
- **Agent tests:** recorded responses from a fake OpenAI-compatible server; a small set of live smoke tests against the real endpoint.
- **Collector tests:** Xvfb with synthetic windows and processes.
- **Scoring:** golden tests with fixed inputs and expected scores.
- **Regression rule:** a stage is not complete until its tests pass and all earlier stage tests still pass.

## 8. Flask Demo UI

**Retired on 2026-11-17** (Stage 5 exit gate); kept under `spikes/demo-ui` for reference.

- A thin API client in Python, kept separate from the backend code.
- Grows with each stage so backend and collector can be exercised in parallel.
- No business logic; everything goes through the public API.
- Retired in Stage 5.

## 9. Collector Research Items

- Which processes carry useful signal: browsers, code editors, terminals, communication apps, media players, games.
- Filtering rules that discard system noise from the process list, and how those rules stay configurable.
- Active browser tab URL (likely needs a browser extension; decide whether it is in scope).
- Idle detection through X11 and how it affects capture scheduling.
- Window-title parsing per application to extract useful context.
- Capture frequency versus vision model budget.

## 10. Risks and Open Items

1. **Model capabilities.** `glm-5.3-flash:cloud` must be verified for vision, tool calling and structured output (Stage 0).
2. **Cloud screenshots.** Raw screens go to a third party. Accepted for the single-user phase; the local vision model plan covers later users.
3. **Server-side decryption.** The server decrypts tenant data to run the agent, so per-tenant keys do not protect against the running service.
4. **pgvector under RLS.** Check index performance with row-level security early.
5. **TensorRT version.** Confirm support for both Volta and Ampere before Stage 6.
6. **Tokenizer for bge-m3.** The C++ backend needs a tokenizer library compatible with the model.
7. **Bulk reschedule threshold** and other approval thresholds are not yet set.
8. **Browser URL capture** is undecided (see 9).

## 11. Out of Scope for Now

- Wayland support.
- Remote multi-device deployment (design allows it; not built).
- Management UI for tenants and admins.
- Local vision model (planned for when other users are added).
- Custom inference models (Stage 6).
- Migration of v1 data.
