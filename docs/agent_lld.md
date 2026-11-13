# TheLife v2: LLMOps, Agent, Memory and Channels Low-Level Design

Status: Draft 2 (review round 3 applied, 2026-11-11). Builds on `prelim_spec.md` §5.3–5.6 and `backend_lld.md` §8 (tool registry), §10 (jobs). Covers Stage 2, plus the Telegram and notification channels. Scoring is in `scoring_lld.md`; MCP is in §11 here, deliberately thin.
Items marked **(proposed)** await confirmation.

---

## 1. Decisions Carried In (review round 2)

| Topic | Decision |
|---|---|
| Agent shape | One main agent with tools. Other LLM work (vision, summarization, fact extraction, score adjustment) runs as background jobs, not agents |
| Proactivity | Both event-driven and schedule-driven, and the agent can schedule its own follow-ups |
| Channels | Web UI chat, Telegram chat, desktop notifications (via the collector), browser notifications (when enabled) |
| Conversation isolation | Web and Telegram have **separate transcripts**, like work chat versus personal messaging, but share all data and long-term memory, and each sees a short digest of the other |
| Chat as data entry | Anything said on any channel can become platform data (tasks, logs, reminders, plan changes). This is the core difference from v1 (§7.4) |
| Conversation unit | One conversation **per channel per local day** |
| Memory | Last 2 conversations verbatim; at least 1 week of compressed conversations; a separate long-term registry of short, referenceable facts in graph form |
| Errors | Quota exhaustion (especially the **weekly limit**) is detected, classified and handled explicitly. On exhaustion the platform falls back to **manual mode** (v1-style) until the weekly reset (Monday 00:00 local), then catches up. No pre-emptive throttling. The fallback provider is a stub for now (local or another offsite provider later, never Ollama cloud) |
| Prompts | Versioned in the repo |
| MCP | Both streamable HTTP and stdio; built only after web, collector and Telegram are stable in daily use |

## 2. Component Map

```
             ┌──────────── triggers ────────────┐
 web chat ─┐ │ events (outbox) │ schedules │ agent-scheduled follow-ups
 telegram ─┼─►  TurnQueue (one active turn per user+channel)
 approvals ┘                       │
                                   ▼
                     AgentRunner (loop, §5)
              ContextBuilder (§6) ──► LlmGateway (§3) ──► Provider(s)
                     │  tool calls                    ▲
                     ▼                                │
              ToolRegistry (backend_lld §8) ── policy, approvals, audit
                     │
                     ▼
              Notifier (§8) ──► desktop (collector) │ browser │ telegram
 background jobs: vision.analyze, memory.*, registry.extract, scores.adjust ──► LlmGateway
```

All LLM traffic goes through `LlmGateway`. Nothing else holds an HTTP client to a model.

## 3. LLM Gateway (LLMOps)

### 3.1 Provider abstraction
```cpp
struct LlmRequest {
  std::string purpose;            // "agent.turn", "vision.analyze", "memory.compress_day", ...
  Priority priority;              // Interactive > Proactive > Vision > Background
  TenantId tenant; std::optional<ConversationId> conversation;
  std::vector<Message> messages;  // text + image parts
  std::vector<ToolSpec> tools; ToolChoice tool_choice;
  std::optional<Json::Value> response_schema;   // structured output
  int max_tokens; float temperature;
  std::string prompt_id, prompt_version;        // §9
};

class ILlmProvider {
  virtual drogon::Task<LlmResponse> complete(const LlmRequest&) = 0;
  virtual drogon::Task<LlmResponse> stream(const LlmRequest&, DeltaSink&) = 0;
  virtual const Capabilities& caps() const = 0;  // tools, json_schema, vision, ctx_tokens
};
```
- `OpenAiCompatProvider`: `/v1/chat/completions` with SSE streaming, `tools`, `response_format: {type: json_schema}`, `image_url` data URIs.
- `StubFallbackProvider`: `caps()` reports nothing available; every call throws `ProviderUnavailable`. It is a placeholder so the routing code exists and is tested.
- Providers are configured in `config/llm.json` (`name, base_url, api_key_env, models{chat, vision}, role: primary|fallback, caps`). The capability values come from the Stage 0 spike, not assumptions.

**Implementation (Stage 2, increment 1):** `backend/src/llm/`.
- Transport is **libcurl** on a dedicated 4-thread pool (`runBlockingOn`), because Drogon 1.9's `HttpClient` cannot stream response bodies. Gateway logic is synchronous on that pool; `Gateway::complete()` / `structured()` are the coroutine entry points.
- Responses with tool calls carry reasoning leaked into `content`, ending in `</think>` (recorded in `tools_single.json`). The provider keeps only the text after the tag, and while streaming it calls `StreamSink::reset()` when the tag appears, so the leaked part is retracted from the UI. A retry after a failure mid-stream also calls `reset()`.
- Provider state lives in `ops.provider_state` (schema `ops`: deployment state, not tenant data, so the "every public table is tenant-scoped" catalog rule stays absolute). The weekly reset instant is computed by Postgres in the configured timezone (`config/llm.json` `weekly_reset`), since libstdc++ 13 has no tzdb.
- Exhausted, disabled or incapable routing ends in `ProviderUnavailable` (with `resets_at` when known): the single "manual mode" signal for callers. `RateLimited` keeps its class so background work can reschedule.
- Configuration: `THELIFE_LLM_CONFIG` (JSON file), `THELIFE_LLM_BASE_URL` (overrides the primary's endpoint, or alone gives a one-provider setup), `THELIFE_LLM_FIXTURE_DIR` (fixture-capture mode). With none of these the gateway is `unconfigured` and the service still starts.
- `GET /api/v1/system/llm-status` returns mode, provider states and the tenant's usage this week (Monday in the user's timezone).

### 3.2 Capability fallbacks
| Missing capability | Fallback |
|---|---|
| Native structured output (**missing on glm-5.3-flash:cloud**: `response_format` and `format` are ignored, see `spikes/llm/REPORT.md`) | **Tool-as-output:** a single `submit_<schema>` tool → validate (valijson) → one repair turn with the validation errors → schema-in-prompt with the same validation → fail as `InvalidOutput` |
| Native tool calling | JSON "action" protocol in the prompt, same validation path **(only if the spike shows it is needed)** |
| Vision | Vision jobs pause; process detections continue |

**Spike-driven rules (Stage 0):**
- Never send `reasoning_effort: "none"`: on this model it pushes the reasoning into `content`. Use `low` for jobs and the default for agent turns. `message.reasoning` is kept only in debug capture.
- A `200` with empty content and no tool calls, or a `finish_reason` outside `stop | tool_calls | length`, is `InvalidOutput` (observed: `finish_reason: "load"` for an invalid request).
- Error bodies are parsed as JSON whatever the `Content-Type` (a 404 arrives as `text/html` with a JSON body).

### 3.3 Error taxonomy
Every provider error is classified into exactly one class. The classification uses HTTP status, headers and **body patterns recorded during the Stage 0 spike** (stored as test fixtures, so the classifier is tested against real responses).

| Class | Typical signal | Handling |
|---|---|---|
| `Transient` | 5xx, connection reset, timeout | Retry 3× with full-jitter backoff (1 s → 20 s), then fail the call |
| `RateLimited` | 429 with short `Retry-After`, or "too many requests" | Wait `Retry-After` (≤ 60 s) for interactive work; reschedule background work |
| `Unclassified` | any non-2xx not matching a known pattern | Fail the call; **fixture-capture mode** stores the redacted body on first sight, so a real weekly-limit body becomes a test fixture the first time it occurs |
| `QuotaExhausted{scope, resets_at}` | 429/402/403 with a body matching a usage-limit pattern (hourly / session / **weekly**) | Provider state change (§3.4). Never retried blindly |
| `ContextTooLong` | 400 + context-length message | ContextBuilder compacts (§6.3) and retries once |
| `AuthFailed` | 401/403 without a quota pattern | Provider → `disabled`, alert the admin, no retries |
| `BadRequest` | other 4xx | Fail, log the request shape (not content) |
| `InvalidOutput` | malformed tool args or schema failure after repair | The agent gets the error as a tool result (self-correct, max 2), jobs fail and retry later |
| `ContentFiltered` | provider refusal flag | Surface it to the user plainly |

### 3.4 Provider state machine and the weekly limit
```
healthy ──RateLimited──► throttled(until) ──time──► healthy
healthy ──QuotaExhausted(weekly)──► exhausted(resets_at) ──resets_at──► probing ──ok──► healthy
                                                         └─(probe fails)──► exhausted(next estimate)
healthy ──AuthFailed──► disabled (manual)
```
- `resets_at` comes from the error body or headers if the provider includes it; otherwise from the configured reset rule **weekly, Monday 00:00 in the tenant's timezone** (the provider resets "Sunday 11:59 PM+ / Monday morning"), with an hourly probe from then on. One cheap 1-token request checks whether the quota is back.
- State is stored in `provider_state` (survives restarts) and exposed at `GET /api/v1/system/llm-status` for the UIs.

**Behaviour while the primary is `exhausted` ("manual mode", like v1):**
1. Requests route to the fallback provider if one is configured and has the capability. Today it is the stub, so they continue to step 2.
2. **Interactive chat:** the web UI switches to its manual entry forms (a banner shows the reset time). Chat answers in **deterministic mode**. The message parser recognises commands (`/today`, `/tasks`, `/done 3`, `/log 45m reading`, `/approve 12`) and calls the tool registry directly with no LLM. Free-form text gets a fixed reply: "Assistant is offline until {resets_at local}; your message is saved and will be answered then." The message is queued as a `deferred_reply` trigger.
3. **Vision:** the collector config returns `enabled=false` for captures; window, idle and process observations continue, and process detections are still derived (no LLM needed).
4. **Background jobs** (summaries, registry extraction, score adjustment) are rescheduled to `resets_at`. Scores computed without the LLM adjustment are marked `provisional` and recomputed later.
5. **Proactive turns** are suppressed, except deterministic reminders (task due, deliverable due), which use templates.
6. **One** notification to the user per exhaustion event, with the reset time.
7. **Catch-up on reset:** deferred replies are answered first, then data that arrived as manual entries during the outage is reviewed for reminders and follow-ups. Then day summaries, registry extraction and provisional score adjustments run, oldest first, at a paced rate. Screenshots from the outage window are gone by design; those hours rely on process detections and manual entries.

### 3.5 Usage accounting (measure only)
- Expected capacity is roughly 3000+ requests/week with normal payloads on the flash model. That comfortably covers planned usage, so there is **no pre-emptive throttling or admission control**. Exhaustion is handled by §3.4 when it happens.
- `llm_usage` counts requests, tokens and images per tenant, per purpose, per day. It is shown in the UI ("this week: 1,240 requests, 41% vision") so unusual burn is visible.
- A soft warning notification fires once a week if projected usage exceeds the configured `weekly_request_estimate` (default 3000). It is information only and changes no behaviour.
- The priority field on `LlmRequest` stays, because it orders the catch-up queue after a reset.

### 3.6 Observability
`llm_calls(id, tenant, purpose, provider, model, prompt_id, prompt_version, priority, tokens_in, tokens_out, images, latency_ms, status, error_class, created_at)`. **No content** by default. A per-tenant `debug_capture` flag stores request and response bodies for 24 h, for prompt debugging on the developer's own tenant.

### 3.7 Testing
- `fake-llm` server (`docker/fake-llm/fake_llm.py`, standard library only): a FIFO script of responses set over `POST /_fake/script`; replays the recorded Stage 0 bodies (`docker/fake-llm/fixtures/`), streams SSE, injects HTTP errors, dropped connections and streams cut mid-way, and records the request bodies it received. A real weekly-limit body joins the fixtures once fixture-capture mode sees one.
- Gateway tests: every error class → expected state transition and handling; exhausted → manual/deterministic mode; reset at Monday 00:00 local; catch-up ordering and pacing.

## 4. Turns, Triggers and Proactivity

### 4.1 Turn inputs
| Source | Example | Channel of the reply |
|---|---|---|
| User message | web or Telegram text | same channel |
| Approval decision | user approved #12 | channel the request came from |
| Event trigger | task missed, deliverable due in 24 h, long off-plan activity, entertainment while due tasks are undone (`scoring_lld.md` §6), yesterday unobserved (`scoring_lld.md` §4.6), detection disputed, collector silent | routed by Notifier (§8) |
| Routine trigger | morning plan, evening review, weekly review; adaptive timing (§4.4) | routed by Notifier |
| Agent follow-up / reminder | the agent called `agent.schedule_followup(at, reason)` earlier, or a reminder the user asked for in chat (§7.4) | routed by Notifier |
| Deferred reply | message received during a quota outage | original channel |

### 4.2 Trigger pipeline
- Domain modules write events to an **outbox** table in the same transaction as the change (`events(kind, entity, payload, created_at)`). The trigger engine (a job) reads the outbox and matches rules in `trigger_rules` (per tenant, with defaults in the repo), producing `agent_triggers(kind, dedupe_key, due_at, payload, status)`.
- `dedupe_key` coalesces bursts (for example 5 missed tasks → one trigger).
- Dispute re-evaluation (spec 5.7) is a trigger with a restricted toolset and **silent** delivery: the agent records its verdict and only messages the user if the verdict changes something.

### 4.3 "Less bothering" policy (Notifier gate) **(decided defaults)**
- The agent decides whether a proactive turn produces a message at all: it can end the turn with `no_message`.
- Hard limits applied after the agent: quiet hours (default 22:30–07:30 local), at most 6 proactive messages/day, at least 45 min between non-urgent ones. Urgent ones (due within 2 h, approval about to expire) bypass the spacing rule but not quiet hours.
- Skipped messages are kept in the conversation, marked `suppressed`, so the agent knows what it chose not to send.

### 4.4 Routines with adaptive timing
```
routines(id, tenant_id, user_id, kind morning|evening|weekly|custom,
         mode adaptive|fixed, default_time, window_start, window_end,
         weekday_mask, day_profiles text[], learned_time, last_fired_at, ...)
```
- **Adaptive** (default): the `routines.learn` job looks at the last 14 applicable days (same day profile, `scoring_lld.md` §3) and estimates the user's actual rhythm: first active input after the night (collector `idle_end` / session start, or the first chat message), last activity before the night, usual weekly-review slot.
  - Morning fires at the **first activity of the day** within `[window_start, window_end]`, or at the learned typical time if no activity is seen by then.
  - Evening fires at learned typical end of activity − 30 min, never inside quiet hours.
  - The learned times are stored with the routine and shown to the user ("evening review now around 21:40, based on the last 2 weeks").
- **Explicit** overrides always win:
  - Chat: "do my weekly review Saturday 10am", "skip the evening review today". The agent calls `routines.update` / `routines.skip_once`.
  - Calendar: a calendar event with `category = routine.<kind>` fixes that occurrence's time and becomes a planned block for scoring.
- Changing a user-set fixed time follows the edit-user-entered rule (approval required); adaptive shifts within the window are automatic and logged.

### 4.5 Concurrency
One active turn per `(user, channel)`. Messages arriving mid-turn are appended and included in the next loop iteration. Proactive triggers wait if a turn is active on that channel.

## 5. Agent Loop

```
run_turn(input):
  ctx = ContextBuilder.build(user, channel, input)          # §6
  for step in 1..MAX_STEPS (8):
     resp = gateway.stream(ctx.request(), sink=channel_stream)
     if resp.tool_calls empty: break
     for call in resp.tool_calls:                           # reads may run in parallel; writes sequential
        result = registry.invoke(call.name, agent_principal(user, conversation), call.args)
        # Allow → result | RequireApproval → {pending_approval_id} | Deny → {error}
        ctx.append_tool_result(call.id, result)
  persist messages (assistant + tool), usage, prompt_version
  deliver final message (channel) or no_message
```
- Limits per turn: 8 steps, 30 tool calls, token cap per turn (config). Hitting a limit ends the turn with a summary of what was done.
- **Approvals:** the stored approval holds the exact tool name, args, the target's `version`, and a human-readable diff. Approve → execute with `If-Match: version`; if the target changed meanwhile, the approval becomes `stale` and the agent is triggered to re-propose. Approvals expire after 72 h (decided 2026-11-13). They can be approved or rejected from the web UI, from Telegram inline buttons, or by command (`/approve 12`).
- **Kill switch** (`tenants.agent_writes_enabled=false`): all agent write tools are denied; read tools and chat still work.
- **Audit:** every tool call gets an `agent_actions` row (tool, args, decision, result summary, conversation, message id).

**Implementation (Stage 2, increment 2):** `backend/src/agent/`, `backend/src/modules/agent.cc`.
- **Turn queue:** one lane per (user, channel); turns of a lane run in order. The context is rebuilt from the database
  before every model call, so a message that arrives mid-turn is seen by the next call. Each turn records
  `seen_until` (the newest message in its last model call); a queued turn whose message a finished turn already saw
  ends as `merged`, so nothing is answered twice and nothing is left unanswered.
- **Provenance:** items created in a turn started by a user message get `origin = user_stated` and
  `source_message_id`; other turns (approval follow-ups, later triggers) create `inferred` items.
- **Policy additions:** the named policy `user_reported_progress` (on `tasks.complete_occurrence` and
  `tasks.skip_occurrence`) allows the call during a turn started by a user message, because the turn's input is
  what the user reported. **Undo:** the agent may delete an item it created within the last 24 h. Everything else
  follows the table in `tool_registry.md` §4. The kill switch is read from `tenants` on every agent write.
- **Approvals:** stored in `approval_requests` and served through the `approvals` view, which reads a pending
  approval past `expires_at` as `expired` (no job needed). Approve runs the stored call as the agent, with
  `expected_version` pinned to the version the agent saw, inside a savepoint of the decision's own transaction:
  decision and effect commit together. A 412 or 404 makes the approval `stale` and nothing runs. Every decision
  (approve or reject) starts an `approval` turn on the channel the request came from, so the agent tells the user
  what happened and re-proposes a stale change if it still makes sense.
- **Audit:** every agent tool call writes `agent_actions` (tool, args, decision, outcome, approval, result summary or
  error, contract version), including calls that were denied, invalid or held. Approve/reject decisions are rows too.
- **Deterministic mode:** messages starting with `/` never reach the model: `/help`, `/today`, `/done N`,
  `/skip N [reason]`, `/tasks`, `/approvals`, `/approve N`, `/reject N [reason]` (numbers refer to the latest
  listing, recomputed each time). While the gateway is in manual mode (or unconfigured), other messages get the
  fixed offline reply with the reset time; they are saved, and answering them on reset arrives with the trigger
  engine (deferred-reply catch-up).
- **Limits:** 8 model calls and 30 tool calls per turn; hitting the step limit ends the turn with a summary of what
  ran. Tool errors go back to the model as results (status, title, JSON-pointer errors) so it can correct itself.

### Web search (Stage 2 addendum, 2026-11-13)
`web.search` and `web.fetch` call Ollama's web search API (`POST https://ollama.com/api/web_search {query,
max_results ≤ 10}`, `POST …/web_fetch {url}`, `Authorization: Bearer $OLLAMA_API_KEY`; config `web_search` in
`config/llm.json`). They are core tools, so they work on every channel (web chat now, Telegram when it exists).
- **When:** only when the user asks for a lookup, or the answer depends on current outside information (news,
  weather, prices, releases, documentation). Never for the user's own data; no personal details in queries
  (prompt rules, `prompts/agent/system.md` 1.2.0).
- **Limits:** 3 searches and 3 fetches per turn (loop), `daily_limit` web calls per user per 24 h (default 50,
  counted from `agent_actions`). Every call is audited with its query.
- **Untrusted content:** results are cleaned (control characters removed, cut on UTF-8 boundaries: 1,200 characters
  per snippet, 6,000 per page) and marked `untrusted`. Once web content is in a turn, **every agent write of that
  turn is held for approval** (policy input `untrustedContext`), with the approval summary saying so, because
  instructions planted in a page could otherwise steer the agent's tools. The kill switch still wins.
- Queries leave the machine (ollama.com). Without a key the tools answer "not configured".

### 5.1 Agent-only tools (beyond domain CRUD tools)
`memory.recall`, `memory.remember`, `memory.update_fact`, `memory.forget` (approval required when the fact was user-stated), `agent.schedule_followup`, `agent.cancel_followup`, `context.now` (current activity, today's plan, deterministic plan progress so far), `history.search` (semantic, 3-month window), `detections.adjudicate` (dispute flow only), `notify.urgent` (marks the final message urgent; rate-limited).

## 6. Context Assembly and Memory

### 6.1 Prompt layout (stable prefix first)
| # | Section | Source | Budget **(proposed)** |
|---|---|---|---|
| 1 | System prompt | `prompts/agent/system.md` @ version | fixed |
| 2 | Tool definitions | registry, `exposure & AGENT` | fixed |
| 3 | Core profile facts | registry: pinned + highest salience | 1.5k tokens |
| 4 | Now block | time, local day, day profile, channel, today's plan, current activity, plan progress so far (deterministic counts, no LLM score), pending approvals, open reminders | 1k |
| 5 | Episodic summaries | last ≥ 7 days, this channel in full, other channel as a short digest; plus **today's cross-channel digest** (§7.3) | 3k |
| 6 | Retrieved | registry facts for entities mentioned + top-k semantic hits (3-month window) | 2k |
| 7 | Working memory | **yesterday's + today's conversation on this channel, verbatim** ("last 2 conversations") | the rest, up to `ctx_tokens − reply reserve` |
| 8 | Input | user message or trigger description | — |

### 6.2 Tiers
| Tier | Content | Built by | Kept |
|---|---|---|---|
| Working | last 2 day-conversations of the channel, verbatim | — | in `messages`, permanent |
| Episodic | per-day summary per channel (~600 tokens), weekly rollup (~300), monthly rollup (~300) | jobs `memory.compress_day` (after day close), `memory.rollup_week`, `memory.rollup_month` | days: ≥ 14 kept (prompt uses ≥ 7), weeks: 12, months: indefinitely |
| Semantic | embeddings of messages, activities, notes, day summaries | job `embeddings.upsert` → `thelife-embedder` over a Unix socket (`backend_lld.md` §2.1; bge-m3 fp16, 1024-d) | 3-month retrieval window, older rows kept but not searched by default |
| Long-term registry | graph of short facts (§6.4) | agent tools + job `registry.extract` | until superseded or forgotten |

**Day close** happens at `day_start_hour` local time. The conversation for that channel and day is closed; a new message opens a new one.

**Implementation (Stage 2, increment 3):** `backend/src/memory/`, `backend/src/modules/memory.cc`.
- **Day close:** the scheduler queues one `day.close` job per user per local day (`ops.day_closes` makes it exactly
  once, and a restart catches up). It closes the day's conversations, marks unfinished routine occurrences
  `missed`, and queues `memory.compress_day` per channel with user messages. Sundays also queue the week rollup and
  decay salience (×0.9 for entities unused for 7 days, pinned exempt); month ends queue the month rollup.
- **One call per channel-day (deviation, decided here):** `registry.extract` is folded into `memory.compress_day`. A
  single structured call (`prompts/jobs/compress_day.md`, reasoning `low`) returns the day summary, topics and up
  to 10 durable facts. Facts with confidence ≥ 0.7 that do not contradict a known fact become `active`
  (`origin = inferred`); the rest are `proposed` and stay out of the prompt. Duplicate statements are ignored.
- **Context sections in use:** core profile (pinned, then by salience; 30 facts), the Now note, day summaries of
  the 7 days before yesterday on the same channel, facts about entities named in the latest user message, then
  yesterday's and today's transcript verbatim. Week and month rollups are stored for the weekly review.
- **Tools:** `memory.remember` (dedupes; `user_stated` from a user's message), `memory.recall` (name, alias and
  full-text match; marks facts used and raises salience), `memory.update_fact` (supersedes; approval for facts the
  user stated), `memory.forget` (named policy `user_stated_fact`), `history.search` (keyword search over messages,
  activities and day summaries, 3-month default window). An approved change keeps the origin of the request.
  Users see and delete facts at `/memory/facts`.
- **Semantic tier (increment 3b):** `history.search` merges keyword and vector hits (reciprocal rank fusion; each
  hit says `keyword`, `semantic` or `both`); `memory.recall` adds facts close in meaning; the context gains
  "Related from earlier (by meaning)": up to 4 items from before yesterday with similarity ≥ 0.50, computed once
  per turn from the user's message. Without the embedder all three quietly use keywords only.
- **Not yet:** overflow compaction (§6.3), the cross-channel digest (with Telegram), and a review flow for
  `proposed` facts. The pgvector + RLS benchmark of `backend_lld.md` §4.5 (2 tenants × 200k vectors) is still to do;
  queries already set `hnsw.iterative_scan = relaxed_order`.
- **Model offline:** memory jobs throw `Defer` on `ProviderUnavailable` (until `resets_at`) or `RateLimited`, so
  they wait in the queue without spending attempts and run in priority order after the reset (§3.4 catch-up).

### 6.3 Overflow handling
If working memory overflows the budget: (1) shrink section 6, (2) replace yesterday's verbatim conversation with its summary plus its last 10 messages, (3) summarize today's oldest turns into a running summary (`messages.kind = rolling_summary`). The verbatim messages always stay in the DB; only the prompt changes.

### 6.4 Long-term registry (graph)
Short, referenceable facts the agent can recall quickly and act on.
```
kg_entities(id, tenant_id, kind person|project|goal|habit|preference|place|app|topic|other,
            name, aliases text[], summary varchar(280), salience real, pinned bool,
            last_used_at, embedding vector(1024), created_by_kind, ...)
kg_facts(id, tenant_id, subject_id, predicate text, object_id NULL, value text NULL,  -- edge or attribute
         statement varchar(280),            -- human-readable: "Prefers deep work before noon"
         confidence real, valid_from, valid_to NULL, superseded_by NULL,
         source_refs jsonb,                 -- message ids, detection ids, day summaries
         created_by_kind, last_used_at)
```
- **Write paths:** the agent (`memory.remember` during a turn); the `registry.extract` job after day close, which proposes new or changed facts from the day's conversations and detections. It needs confidence ≥ 0.7 and must not contradict a user-stated fact, otherwise the item goes to a review list.
- **Read paths:** pinned plus top-salience facts in every prompt (section 3); entity-name and alias match on the input, plus 1-hop neighbours (section 6); embedding similarity over entity summaries.
- **Salience** = recency of use × frequency × user-pinned boost. It decays weekly (job `registry.decay`).
- Facts are append-only like detections: a change creates a new fact with `superseded_by` on the old one.
- `memory.forget` on a user-stated fact requires approval (spec 5.3).

## 7. Channels

### 7.1 Web chat
- `POST /api/v1/chat/web/messages {text, client_msg_id}` returns `202 {turn_id}`. `GET /api/v1/chat/web/stream` is a per-user SSE stream of `turn.started | token | tool.started | tool.finished | approval.requested | turn.finished | notification`.
- `GET /api/v1/conversations?channel=&from=&to=` and `GET /api/v1/conversations/{id}/messages` for history.
- **Implemented (increment 2):** `POST /chat/web/messages {text, client_msg_id?}` (the id, or `Idempotency-Key`,
  makes resends return the original turn), `GET /chat/turns/{id}` for clients without the stream, and the stream
  events `turn.started`, `token`, `reset` (retract streamed text: a retry, or leaked reasoning), `tool.started`,
  `tool.finished`, `approval.requested`, `message`, `turn.finished`, with a comment heartbeat every 20 s.

### 7.2 Telegram
- **Long polling** (`getUpdates`) from the backend, so there is no public webhook endpoint and nothing inbound to expose. Bot token from env.
- **Linking:** the web UI issues a one-time code (10 min); the user sends `/link <code>` to the bot; the backend stores `telegram_links(user_id, chat_id, linked_at, revoked_at)`.
- Only **private chats** from linked `chat_id`s are processed. Everything else is dropped without a reply (no information leak) and counted.
- Messages → `channel=telegram` conversation. Replies are sent after the turn finishes (with a "typing…" chat action meanwhile), converted to MarkdownV2 with escaping.
- Approvals use inline keyboards. `callback_data` = `ap:<approval_id>:<HMAC(user_id, approval_id)>` truncated to the 64-byte limit, and it is verified before acting.
- Deterministic commands (§3.4) work here too, which matters most when the LLM is down and you are away from home.
- Telegram is a first-class **data entry** channel (§7.4): everything logged there shows up in the platform immediately.
- Privacy note: Telegram bot chats are not end-to-end encrypted, so message content passes through Telegram's servers.

### 7.3 Isolation semantics
| Shared across channels | Isolated per channel |
|---|---|
| All domain data, detections, scores | Conversation transcripts (working memory) |
| Long-term registry | Per-day summaries (the other channel appears only as a short digest) |
| Pending approvals (actionable from either) | Streaming and typing state |
| Today's cross-channel digest (both directions) | |

- **Cross-channel digest (decided):** each channel's turn sees a rolling digest of the *other* channel's conversation today: topics, decisions, data changed, open follow-ups, about 200 tokens. It is refreshed by a cheap job after each turn that changed data or ran 6+ messages; outside those cases the last digest is reused. Example: on web in the evening the agent knows "on Telegram at 14:10 you logged 40 min gym and asked to be reminded to call the bank at 18:00".

### 7.4 Chat as data entry (core differentiator)
Any message on any channel may carry data. The agent's job on every user turn is to **capture first, then converse**:
1. Extract actionable items: activity logs ("did 45 min of the Rust course, section 4"), tasks ("need to renew passport by Dec"), reminders ("remind me at 6 to call the bank"), plan changes ("moving gym to tomorrow"), facts ("I'm switching to the night shift next month"), work logs on non-observed devices (`scoring_lld.md` §5).
2. Write them with the normal tools (`activities.create`, `tasks.create`, `reminders.create`, `skills.sessions.create`, `memory.remember`, …).
3. Reply with a short **receipt** of what was recorded, each item linkable or undoable: "Logged: Rust course §4, 45 min (skill). Reminder: call bank, 18:00. Reply 'undo 2' to drop the reminder."

**Provenance:** items created this way are `created_by_kind=agent` with `origin=user_stated` and `source_message_id`. The policy engine treats `origin=user_stated` items as **user-entered** for later edits (approval needed), because they encode the user's intent, not the agent's guess. `undo` within 24 h through the receipt is allowed without approval.

**Ambiguity:** if a required field cannot be inferred (which task? which day?), the agent asks one short clarifying question instead of guessing, except for time, where it uses "now" or the closest sensible slot and says so in the receipt.

**Reminders and follow-ups:**
```
reminders(id, tenant_id, user_id, text, due_at, rrule NULL, channel_pref NULL,
          origin, source_message_id, status pending|sent|done|cancelled, link_type, link_id)
```
Due reminders become Notifier deliveries through the template path, so they need **no LLM call** and still work in manual mode. A reminder linked to a task is also checked at its due time: if the task is already done, the reminder is cancelled silently. "Follow back" items (`agent.schedule_followup`) differ from reminders: they start an agent turn, so the agent can ask how something went.

## 8. Notifier and Notification Routing

```
notifications(id, tenant_id, user_id, kind, urgency, title, body,
              conversation_id, message_id, ref_type, ref_id, created_at, resolved_at)
notification_deliveries(notification_id, channel desktop|browser|telegram,
                        status queued|sent|shown|clicked|failed|skipped, at)
```
Routing, evaluated per notification:
1. **Desktop** if a collector heartbeat is fresh (< 2 min) and the user is not idle or locked. The collector long-polls and acks (`collector_lld.md` §7.5).
2. **Browser** if a web UI tab holds the SSE stream (Notification API in the tab). Web Push with VAPID for closed tabs comes later **(proposed)**.
3. **Telegram** if neither of the above acks within 3 min, or immediately when the user is away (no fresh heartbeat).

**Implemented so far (increment 2):** the `notifications` and `notification_deliveries` tables, and the collector's
long poll (`GET /devices/self/notifications?wait=`, repeated in short transactions by the REST adapter) and ack. The
Notifier that creates notifications and routes them arrives with triggers and reminders.

An ack on any channel resolves the notification, so it is not delivered again elsewhere. The message content lives in the conversation of the channel where it was **first delivered**. A desktop click opens the web chat, so desktop and browser deliveries belong to the web conversation.

## 9. Prompts in the Repo

```
prompts/
  agent/system.md                 # main agent
  agent/triggers/*.md             # per-trigger preambles (morning, evening, weekly, dispute, ...)
  jobs/vision_analyze.md  jobs/compress_day.md  jobs/rollup_week.md
  jobs/registry_extract.md  jobs/score_adjust.md
  schemas/*.json                  # structured-output schemas used by jobs
```
- Each file has front matter `id`, `version` (semver), `model_hint`, `variables`. Templates use a minimal `{{var}}` substitution with no logic, implemented in-tree.
- Prompts are loaded at startup; the content hash plus version is recorded on every `llm_calls` and `messages` row.
- **Prompt eval tests** run each prompt against recorded fixtures through `fake-llm` in CI for format, and against the live endpoint in a manual `make eval-live` run. A prompt version bump is reviewed like code.

## 10. New Tables (Stage 2)

`conversations(user_id, channel, local_day, status open|closed, closed_at)` · `messages(conversation_id, role, kind normal|rolling_summary|suppressed|deterministic, content, tool_calls, tool_call_id, prompt_version, tokens)` · `episodic_summaries(user_id, channel, period day|week|month, period_start, content, model, prompt_version)` · `kg_entities`, `kg_facts` · `agent_triggers`, `trigger_rules`, `agent_followups` · `events` (outbox) · `notifications`, `notification_deliveries` · `telegram_links`, `link_codes` · `reminders` · `routines` · `channel_digests(user_id, channel, local_day, content, updated_at)` · `llm_calls`, `llm_usage`, `provider_state` · `approvals` (extends backend_lld §6.7).

Every table follows the RLS pattern and catalog tests in `backend_lld.md` §4.

## 11. External Agents via MCP (deferred, contract only)

Built only after the web UI, collector and Telegram are stable in daily use.
- One MCP server module in the backend exposing registry tools with `exposure & MCP`, over **streamable HTTP** at `/mcp`, authenticated by a scoped API key.
- A tiny **stdio shim** binary (`thelife-mcp`) for clients that only speak stdio. It forwards to `/mcp` with the API key from env. No logic of its own.
- Reads are allowed by scope. Writes become proposals that the main agent adjudicates (spec 5.6). `add_context` writes into the long-term registry as `created_by_kind=external`, with low default salience.

## 12. Testing Summary (Stage 2 exit gate additions)

- Gateway: every error class incl. recorded weekly-limit bodies; state machine; budget admission; deterministic mode.
- Agent: scripted fake-LLM conversations covering tool use, approval required → approve → executed, stale approval, kill switch, step limits, `no_message` proactive turns.
- Memory: day close → summary → appears in next-day context; overflow compaction order; registry supersede; `forget` gated.
- Chat capture: recorded fixtures for logs, tasks, reminders, plan changes → correct tool calls, receipt content, `origin=user_stated` provenance, undo within 24 h, edit later requires approval.
- Routines: adaptive time from synthetic activity history; chat override; calendar override; skip once.
- Channels: Telegram unlinked chat ignored, HMAC callback tampering rejected, isolation (Telegram turn never sees web transcript verbatim).
- Notifier: routing matrix, ack de-duplication, quiet hours, daily cap.
- Live smoke (manual / nightly): one real turn, one vision call, one structured-output job against `glm-5.3-flash:cloud`. Implemented so far: streamed turn, tool call, structured output (`backend/tests/live/`, runs only with `THELIFE_LIVE_LLM_URL`); the vision call arrives with the vision pipeline.

## 13. Decisions (review round 3, 2026-11-11)

1. Proactive limits: quiet hours 22:30–07:30, max 6/day, 45 min spacing. Accepted.
2. Cross-channel: both directions get a daily digest; Telegram is a full data entry channel (§7.3, §7.4).
3. Routines adapt to detected patterns, with explicit overrides through chat or calendar booking (§4.4).
4. No budget throttling. On exhaustion: manual mode until Monday 00:00 local, then catch up (§3.4, §3.5).
