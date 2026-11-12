# TheLife v2: Tool Registry and Contract Spec

Status: Draft 1 (Stage 0, 2026-11-12). Implements design principle 1 (one tool registry) and 2 (API first) of `prelim_spec.md`. Builds on `backend_lld.md` §7–8 and `agent_lld.md` §5.

---

## 1. One Definition, Three Surfaces

```
api/common.yaml + api/resources/*.yaml          (hand-written source of truth)
                 │  api/tools/contractgen.py
     ┌───────────┼──────────────────────────┐
     ▼           ▼                          ▼
generated/openapi.yaml   generated/tools.json        generated/agent_tools.json
(public REST contract)   (embedded in the backend:  (OpenAI tool format for the agent
                          registry manifest)          and the prompt-eval suite)
```
- The generated files are committed so reviewers see contract diffs. CI runs `contractgen.py --check` and fails on any drift (`.github/workflows/contracts.yml`).
- `generated/openapi.yaml` is validated against OpenAPI 3.1, and every tool's input and output schema against JSON Schema 2020-12 (`api/tools/validate.py`).
- MCP (Stage 3) reads the same `tools.json`, filtered by `exposure: mcp`.

## 2. Source Format

### 2.1 `common.yaml`
`info`, `schemas` (shared types such as `Id`, `Timestamp`, `LocalDate`, `Area`, `Priority`, `Origin`, `ResourceMeta`, `Problem`, `PendingDecision`), `parameters` (`IfMatch`, `IdempotencyKey`, `IdempotencyKeyRequired`), `headers`, `responses` (problem responses).
References use the short form `{$ref: Name}`. Sibling keys such as `description` or `default` are allowed and merged.

### 2.2 `resources/<name>.yaml`
| Key | Meaning |
|---|---|
| `name` | Tool prefix (`tasks` → `tasks.list`, …) |
| `schema` | Resource schema name. The generator derives `<S>` (with `ResourceMeta`), `<S>Create`, `<S>Patch` (no required, no defaults, `minProperties: 1`) and `<S>Page` |
| `path` | Collection path under `/api/v1` |
| `scope` | Scope family: reads need `<scope>:read`, writes `<scope>:write` |
| `fields` / `required` / `readonly` | Writable properties, required-on-create, and server-set properties |
| `filters` | Extra list query parameters (added to `limit`, `cursor`, `updated_since`, `include_deleted`) |
| `ops` | Standard operations: any of `list, get, create, update, delete`, each with optional overrides |
| `actions` | Custom operations (below) |
| `extra_schemas` | Additional named schemas |
| `exposure`, `principals`, `stage` | Defaults for all tools in the file |

Action keys: `name` (a dot makes it absolute, for example `calendar.view`), `method`, `path` (relative to `/api/v1` unless `absolute: true`), `effect`, `summary`, `params` (path), `query`, `body`, `returns`, `status`, `target`, `scopes`, `principals`, `exposure`, `stage`, `public`, `tool: false` (REST-only plumbing such as login and health), `idempotency_required`, `content_type`, `extra_responses`, `policy`.

## 3. Tool Semantics (`tools.json`)

```json
{ "name": "tasks.update", "description": "...", "effect": "update",
  "scopes": ["tasks:write"], "exposure": ["rest","agent","mcp"], "principals": ["user","agent","api_key"],
  "stage": 1, "target": {"resource": "Task", "id_arg": "id"}, "policy": null,
  "input_schema": {...}, "output_schema": {...},
  "rest": {"method": "PATCH", "path": "/api/v1/tasks/{id}", "success_status": 200,
           "path_args": ["id"], "query_args": [], "body_args": ["title", ...], "if_match": true} }
```
- **Arguments** are the union of path parameters, query parameters and body properties (name clashes are a generator error). Update and delete tools also take `expected_version`: required as `If-Match` over REST, optional for the agent (approvals pin it, `agent_lld.md` §5).
- **Effect** says what the call does to stored data: `read`, `create` (adds records only, including append-only events such as detection confirm, dispute or claim), `update`, `delete` (always soft).
- **Target**: update and delete tools exposed to the agent must name a target or an explicit `policy`. The generator enforces this, so the policy engine can always inspect the target's provenance.
- **Exposure:** `rest`, `agent`, `mcp`. **Principals:** `user`, `agent`, `api_key`, `device`. Device tools must be REST-only.
- **Agent names** map `.` → `_` (`tasks.update` → `tasks_update`); the generator rejects collisions.

## 4. Permission Policy (machine form of spec §5.3)

`Policy::decide(principal, tool, target)` is evaluated by the registry for every call, from REST or from the agent:

| Principal | read | create | update | delete |
|---|---|---|---|---|
| `user` (session token) | allow | allow | allow | allow |
| `api_key` (non-MCP) | allow if scope | allow if scope | allow if scope | allow if scope |
| `api_key` (MCP client) | allow if scope | **propose** | **propose** | **propose** |
| `agent` | allow | allow + audit | allow + audit if `target.created_by_kind = agent` and `target.origin ≠ user_stated`, or an undo within 24 h of a receipt; otherwise **approval** | **approval** |
| `device` | only tools listing `device` in principals | | | |
| any principal not in `tool.principals` | deny (403) | | | |
| `agent` while the tenant kill switch is on | allow | deny | deny | deny |

- `approval` and `propose` answer `202 PendingDecision` and execute nothing.
- `tool.policy` names an extra rule evaluated after the table. Reserved names, implemented in Stage 2:
  - `bulk_threshold`: approval when one call affects more than the tenant's `agent.bulk_threshold` items.
  - `user_stated_fact`: approval to forget a user-stated memory fact.
- Every non-user call writes an `agent_actions` audit row: tool, args, decision, result summary, principal and conversation.
- **Open for Stage 2:** `tasks.complete_occurrence` and `tasks.skip_occurrence` are `update` effects on the Task. Under the table, an agent logging "done" for a routine the user created would need approval, which is wrong for chat-as-data-entry. A named policy such as `user_reported_progress` (allow and audit when the triggering message states it) is the likely fix.

## 5. Backend Integration (Stage 1)

- `tools.json` is embedded into the binary at build time (CMake turns it into a generated source file), so the binary and its contract cannot drift. `GET /healthz` will report `contract_version`.
- Each module registers handlers by tool name: `registry.bind("tasks.update", &TasksService::update)`. **At startup the registry fails if any stage ≤ current-stage tool has no handler, or a handler names an unknown tool.** Active since Stage 1 (`build::kStage = 1`, CMake `THELIFE_STAGE`): the server refuses to start with an unimplemented Stage 1 tool.
- Contract CRUD resources bind through `crud::bindCrud` (table columns named like contract fields). Custom tools (occurrences, calendar view, detection actions, credential issuing) bind plain handlers. Update and delete tools that target an entity also pass a target resolver, which the agent policy uses for provenance.
- A generic REST adapter builds routes from the `rest` mapping:
  1. Collect path, query and body arguments into one args object.
  2. Validate against `input_schema` (valijson) → 422 problem.
  3. Run `Policy::decide`, invoke the handler inside a `TenantTx`, and map the result to the response.

  Controllers only exist for endpoints marked `tool: false` (health, login, logout, profile, password).
- Tests derived from the manifest (Stage 1; the first three implemented in `backend/tests/api/contract_test.cc`, resource lifecycles in `resources_test.cc`):
  - every REST tool is called with a token lacking its scope (expect 403) and with a device token (expect 403 unless the tool allows devices)
  - every target tool answers 404 for another tenant's id
  - every update or delete without `If-Match` answers 428
  - schemathesis checks responses against `openapi.yaml`

## 6. Agent Tool Budget (decided 2026-11-12: tool groups with on-demand loading)

The full agent surface is **81 tools ≈ 11k tokens** per call (measured from `agent_tools.json`). That is too large to send on every turn, and tool-choice accuracy drops with that many options.

**Decision: tool groups with on-demand loading.** The generator gains `agent_group` and emits a groups index in Stage 2.
- Each resource file gets `agent_group` (`core`, `planning`, `work`, `skills`, `entertainment`, `detections`, `admin`).
- Every turn sends `core`, about 15 tools / 2.5k tokens:
  - capture: `activities.create`, `tasks.create`, `tasks.update`, `reminders.create`, `skill_sessions.create`
  - `calendar.view`, `context.now`, `memory.recall` / `memory.remember`, `history.search`
  - `detections.claim` / `detections.dispute`
  - `agent.load_tools(group)`
- When the agent calls `agent.load_tools("work")`, the next loop iteration includes that group's tools for the rest of the turn.
- Trigger preambles preload the groups they need (the weekly review preloads `planning` and `detections`).

Rejected alternative: generic `records.list|get|create|update(type, …)` tools with per-type schemas loaded on demand. This costs fewer tokens, but the model has to fill a free-form `fields` object, which validates worse.

## 7. Change Rules

- Additive changes (new optional field, new tool, new filter) stay in `/api/v1`. Renaming or removing anything, or adding a required field, needs `/api/v2` or a deprecation window.
- `contract_version` (hash of the tool manifest) is recorded on `agent_actions` and `llm_calls`, so behaviour can be traced to the contract in force.
- Stage 2+ resources (reminders, routines, targets, day profiles, work contexts, scores, chat, approvals, notifications, conversations, memory, agent-only tools) are added to `api/resources/` at the start of Stage 2. Stage 4 device endpoints are already present so the collector contract is fixed.
