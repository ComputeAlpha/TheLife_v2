# TheLife v2: Scoring Low-Level Design

Status: Draft 2 (review round 4 applied, 2026-11-11). A new model, deliberately not a port of v1. Builds on `prelim_spec.md` §5.7, `backend_lld.md` §6.5 (detections) and `agent_lld.md` (triggers, disputes, LLM gateway). Items marked **(proposed)** await confirmation.

---

## 1. Decisions Carried In (review round 3)

| Topic | Decision |
|---|---|
| What the score rewards | **Both** plan adherence and quantity against targets, with configurable weighting between them and a separate consistency component |
| Fitness and sleep | Manual or Telegram logs only (no wearables for now) |
| Entertainment | No limits and no score weight. The agent watches it, attributes missed or preempted items to it, and warns when tasks are due or undone |
| Weekends and rest days | Per-item applicability: some items stay daily including weekends, others (especially professional work) differ |
| Work evidence | Must adapt: today work happens on this observed machine; a future employer may require a dedicated or restricted device, where work is logged concisely through chat with no screenshot evidence |
| LLM adjustment | ±30 %, always with stated reasons; disputes are recorded to learn patterns and frequency |
| Cadence | **End of day only.** No intraday LLM scoring; the agent sees deterministic plan progress during the day |
| Guiding principle | Targets are not always reached; **the biggest failure is giving up on the flow**. Showing up for the plan is weighted far above volume |

## 2. Pipeline

```
        day closes at day_start_hour (local, default 04:00 next morning)
                                │
 inputs snapshot ──► deterministic base score (§4) ──► preemption analysis (§6)
 (tasks, blocks, targets,                         │
  detections, activities,                         ▼
  skill sessions, logs)            1 LLM call: adjustment ±30 % (§8)
                                                  │
                                                  ▼
                       scores row (final) ──► agent evening/morning summary uses it
```
- Job `scores.close_day(user, local_day)` runs once per user per day. It makes **one** LLM call per day, plus at most one re-run per day for disputes or significant late changes (§9).
- During manual mode (LLM exhausted) the row is stored as `provisional` with base only; the adjustment runs during catch-up (`agent_lld.md` §3.4).

## 3. Day Profiles and Applicability

```
day_profiles(id, tenant_id, name, scored bool, freezes_streaks bool)
  -- defaults: workday(scored), weekend(scored), holiday(scored), leave(not scored, freezes), sick(not scored, freezes)
user_settings.workdays = Mon..Fri                  -- default mapping weekday → workday|weekend
day_profile_overrides(user_id, local_day, profile_id, origin, source_message_id)
```
- The profile for a day comes from the override if one exists, otherwise from the weekday mapping.
- Overrides come from chat ("tomorrow I'm on leave"), an all-day calendar event with `category = day_profile.<name>`, or the UI.
- Every recurring task, routine and target has `day_profiles text[]` (default: all scored profiles). RRULE decides *which dates*; profiles decide *which kinds of day*. Examples:
  - "Read 20 pages": every day, all profiles → stays consistent on weekends.
  - "Work 6 h": `workday` only.
  - "Gym": Mon/Wed/Fri RRULE, workday + holiday.
- An area with no applicable planned items and no applicable targets on a day is **not applicable** that day, and its weight is spread over the other areas (§4.4). A weekend without planned work does not score 0 for work.
- `leave` / `sick` days are not scored, are excluded from period averages, and do not break streaks.

## 4. Base Score (deterministic)

Areas: `work`, `skill`, `fitness`, `personal` (each scored from plan + targets), and `consistency` (cross-area). `entertainment` and `other` have weight 0.

### 4.1 Plan adherence (per area *a*)
Planned set *P_a* for the day:
- task occurrences due that day (including recurring ones applicable to the profile)
- calendar time blocks with area *a* (including routine blocks)
- deliverable milestones due that day

Each item *i* has a weight *w_i*: task priority low 1 / normal 1.5 / high 2 / critical 3; time block 1 per planned hour (min 0.5).
Completion *c_i* ∈ [0, 1]:
| Item | c_i |
|---|---|
| Task done that day | 1 |
| Task done within 24 h after day close (late entry or late completion) | 0.5 |
| Time block | minutes of matching evidence inside the block ÷ block minutes, capped at 1 |
| Deliverable milestone met | 1 |
| Not done | 0 |

**Replanning is not failure:** an item rescheduled or cancelled *before* its start (by the user, by chat, or by the agent with approval) leaves *P_a*. Skipping after the fact keeps it with *c_i = 0*.

`adherence_a = Σ w_i·c_i / Σ w_i`

### 4.2 Quantity against targets (per area *a*)
```
targets(id, tenant_id, user_id, area, metric minutes|pages|sections|sessions|count,
        amount, period day|week, day_profiles text[], goal_id NULL,
        match jsonb,              -- which evidence counts: category, linked resource, project
        track_consistency bool, active_from, active_to)
```
For daily targets: `q_t = min(achieved_t / amount_t, 1.0)` and `over_t = max(0, achieved_t / amount_t − 1)`.
`quantity_a = mean(q_t)` over applicable daily targets. Weekly targets are scored at the week level (§10).

### 4.3 Area score
```
bonus_a = min(5, 25 · mean(over_t))          -- 20 % over target → +5 points, the maximum
score_a = min(100, 100 · (α_a·adherence_a + β_a·quantity_a) + bonus_a)
```
with α_a + β_a = 1.
- Defaults **(decided)**: **α = 0.8 / β = 0.2** for all areas, so staying in the flow counts far more than volume. Per-area overrides live in `scoring_settings`.
- Over-achievement bonus **(decided)**: up to +5 points per area, and the area score is still capped at 100. A day of sticking to the plan can reach 100 without any bonus; the bonus mostly helps a day with a partly-missed plan but strong output.
- Only planned items → `score_a = 100·adherence_a`. Only targets → `min(100, 100·quantity_a + bonus_a)`. Neither → area not applicable.

### 4.4 Consistency (cross-area)
Tracked set *C*: recurring tasks, routines and daily targets with `track_consistency = true`.
For each item *k*: `r_k` = completions ÷ applicable days over the trailing 7 applicable days, including today (leave, sick and unobserved days skipped; an unobserved day re-enters once context arrives).
`score_consistency = 100 · mean(r_k)`. Streak lengths are stored and shown, but do not change the number **(proposed)**, so a single miss does not cause a cliff.

### 4.5 Combining
```
W = {work 0.30, skill 0.25, fitness 0.15, personal 0.15, consistency 0.15}   -- scoring_settings, per user
base = Σ_{a applicable} W_a · score_a  /  Σ_{a applicable} W_a
```

### 4.6 Unobserved days **(decided)**
- A scored-profile day with **no evidence of any kind** is marked `unobserved`. No evidence means no detections, collector observations, activities, logs, task completions or chat messages. This applies whether or not the day had a plan, because a missing signal does not prove failure.
- An unobserved day gets a `scores` row with `status = unobserved`, no base and no final. It is excluded from period averages and counted in period coverage. For consistency it is treated as not applicable for now (§4.4), until context arrives.
- **The agent asks for context:** the trigger `day_unobserved` fires at the first activity of the next day (morning routine, or the first chat on any channel). The agent asks what happened ("No signal from yesterday: you had the gym block and 2 tasks planned. What did the day look like?").
  - Answers become normal chat-captured data (`agent_lld.md` §7.4): activity logs, completions, or a day-profile override (sick, leave, holiday).
  - The day is then recomputed (§9) and becomes a normal scored day, or stays excluded under the new profile.
  - If there is no answer, the agent asks once more within 3 days, then leaves the day as `unobserved` and stops asking.
- Unobserved days are tracked like disputes (§7): more than 2 in a rolling 4 weeks becomes a pattern, raised in the weekly review. A silent day can itself be a sign of having dropped out of the flow.

### 4.7 Evidence rules
- Evidence = non-superseded detections (`detected` or `confirmed`) with confidence ≥ 0.4 **(decided)**, plus activities, skill sessions and manual or chat logs.
- Disputed detections are excluded until re-evaluated (spec 5.7).
- Overlapping evidence per minute is resolved by the decided precedence `user > vision > process > external` (`backend_lld.md` §6.5), so the same minute never counts twice.
- Evidence matches a time block when the area matches and, if the block links an entity (course, project, book), the evidence links to or is categorised as that entity.
- The agent may mark a task done from evidence (`origin=inferred`). It counts, and the user can dispute it like a detection.

## 5. Work Contexts (observed vs self-reported)

```
work_contexts(id, tenant_id, user_id, name, employer,
              evidence_mode observed|self_reported,
              device_ids uuid[],          -- observed devices that carry this work
              workday_target_min DEFAULT 360, active_from, active_to)
```
- **observed** (today: same machine for work and personal): work evidence comes from detections on `device_ids`, plus chat logs.
- **self_reported** (future dedicated or restricted device): nothing is captured for that work. Evidence comes from concise chat logs ("work: 9–13 sprint planning, 14–18 API review"), deliverable updates and calendar blocks.
  - The evening routine asks for a one-line work log on workdays without one. The agent turns it into `activities(area=work, origin=user_stated)`.
- Self-reported and observed evidence count **equally** in §4. Trust is checked through dispute and pattern tracking (§7), not by discounting.
- **Work is scored from both plan and target (decided).** Each active work context creates a daily `work` target (`workday_target_min`, default 6 h, `workday` profile only) next to the planned work blocks and deliverables. With α = 0.8, missing planned work blocks costs much more than falling short on hours.
- Switching employers = closing one context (`active_to`) and opening another. History keeps its original mode. Classifier rules on the personal machine can mark the old employer's apps as `other` from then on.

## 6. Entertainment Watch and Preemption

Entertainment is unlimited and weightless. What gets tracked is **what it displaced**.

### 6.1 Intraday warning (event trigger, no score)
- Fires when entertainment evidence is ≥ 15 min continuous **(proposed)** and either:
  - a planned block is active right now, or
  - tasks due today or overdue are undone, or
  - a deliverable is due within 24 h.
- This starts an agent turn (`agent_lld.md` §4.1) with the specific items. The agent decides tone and whether to message, within the Notifier limits. At most one warning per entertainment session; it is urgent only if something is due within 2 h.

### 6.2 Preemption attribution (at day close)
```
preemptions(id, tenant_id, user_id, local_day, entertainment_ref, displaced_type, displaced_id,
            minutes, kind overlap|while_due)
```
- `overlap`: entertainment evidence inside a planned block that ended up with *c_i < 1*.
- `while_due`: entertainment while an item due that day was undone and remained undone at close.
- Preemptions **do not subtract points directly**, because the missed item already lowered adherence. They are used for:
  - the explanation ("Gym block missed: 50 min of gaming overlapped it")
  - an LLM adjustment input (§8)
  - weekly review statistics ("entertainment displaced 3 planned items this week")
  - registry facts when a pattern holds for 3+ weeks ("Evening gaming tends to displace reading")

## 7. Disputes and Pattern Tracking

- A dispute never lowers the score by itself. The disputed evidence is excluded until the agent re-evaluates it (silent trigger, `agent_lld.md` §4.2). The re-evaluation can uphold, revise (new superseding detection) or accept the user's claim.
- `dispute_stats` (a view over `detection_events` and score-adjustment disputes), rolling 4 weeks:
  - dispute rate per category and per source
  - **self-favourable ratio**: the share of disputes whose resolution would raise the score
  - share of disputes overturned by the agent
  - time-of-day and day-of-week clustering
- **Flags** **(proposed)**: more than 5 disputes and a rate above 25 % in a category, or a self-favourable ratio above 0.8 across 10+ disputes. A flag is:
  - recorded as a registry fact (`habit`)
  - passed to the LLM adjustment
  - raised by the agent in the weekly review, never as an accusation mid-day
- Flags are information. Nothing is penalised automatically.

## 8. LLM Adjustment (±30 %)

- One structured-output call at day close (`prompts/jobs/score_adjust.md`). Input:
  - the base breakdown
  - active goals and plans
  - preemptions
  - dispute flags
  - the day's summary
  - work-context mode
- Output schema:
  ```json
  {"adjustment_pct": -30..30,
   "reasons": [{"text": "...", "direction": "up|down", "refs": [{"type": "goal|task|preemption|...", "id": "..."}]}],
   "goal_alignment": "low|medium|high"}
  ```
  At least one reason is required whenever `adjustment_pct ≠ 0`; the output is rejected and repaired otherwise.
- `final = clamp(round(base · (1 + adjustment_pct/100)), 0, 100)`.
- The adjustment is meant to capture **long-term goal alignment** that the formula cannot see (for example a low-adherence day spent on an unplanned but goal-critical deliverable).
- The user can dispute the adjustment (`POST /scores/{id}:dispute {reason}`). The agent re-runs it once with the reason; the outcome goes into `dispute_stats`.

## 9. Recompute and Versioning

```
scores(id, tenant_id, user_id, period day|week|month|year, period_start,
       profile, formula_version, inputs_hash,
       breakdown jsonb,          -- per area: adherence, quantity, α/β, items[] with w_i and c_i, targets[]
       consistency jsonb, preemptions_ref, base numeric(5,2),
       adjustment_pct numeric(4,1) NULL, adjustment jsonb NULL,
       final numeric(5,2) NULL, status provisional|final|unobserved, is_current bool, created_at)
```
- Rows are append-only; the newest becomes `is_current`.
- Late changes to a closed day (late log, dispute resolution, late completion) → `scores.recompute(day)`, debounced 10 min. The base is recomputed for free. **The adjustment is reused** unless the base moves by more than 5 points or a dispute concerns the adjustment itself. This keeps LLM use at about one call per day.
- `formula_version` lets a formula change be re-applied to history in a job, without mixing versions in charts.

## 10. Periods

- **Week** (Mon–Sun), **month**, **year**: `period_score = mean(final)` over scored days (leave and sick excluded), shown with coverage (scored days ÷ calendar days).
- **Weekly targets** (for example "gym 3×/week") **(decided)**: if any apply, `week_score = 0.8 · mean(daily final) + 0.2 · 100 · mean(min(achieved/amount, 1))`.
- Period scores are computed deterministically at period close with no extra LLM call. The weekly review conversation uses them.

## 11. API (additions to `backend_lld.md` §7.1)

| Endpoint | Purpose |
|---|---|
| `GET /scores?period=day&from&to` | current rows (breakdown included) |
| `GET /scores/{id}/history` | all versions of that period |
| `POST /scores/{id}:dispute {reason}` | dispute the adjustment |
| `GET /scores/today/progress` | deterministic plan progress so far (no LLM, no score number) |
| CRUD `/targets`, `/day-profiles`, `/day-profile-overrides`, `/work-contexts`; `GET/PATCH /settings/scoring` | configuration |
| `GET /insights/preemptions?from&to`, `GET /insights/disputes` | review data |

## 12. Testing

- **Golden tests:** JSON day fixtures (plan, targets, evidence, profile) → expected breakdown and base, checked to two decimals. Includes:
  - workday
  - weekend with consistent items
  - leave day
  - self-reported work
  - replanned-before-start item
  - overlapping evidence precedence
  - disputed evidence excluded
  - unobserved day → context answered → rescored
  - over-target bonus capped at +5 and area at 100
- **Property tests:**
  - 0 ≤ base, final ≤ 100
  - completing an item never lowers the base
  - a non-applicable area never changes the base
  - recompute with unchanged inputs is idempotent (same `inputs_hash`, no new row)
- **Adjustment:** fake-LLM fixtures for valid, out-of-range (clamped and repaired) and missing-reason (rejected) outputs; provisional → final on catch-up.
- **Preemption and warning:** synthetic entertainment detections over blocks and due tasks → expected rows and trigger events.
- **Dispute stats:** synthetic dispute histories → expected flags.

## 12a. Implementation (Stage 2, increment 4)

`backend/src/scoring/`: `engine.*` (pure; golden and property tests in `unit/scoring_test.cc`), `inputs.*` (reads a
user-day), `jobs.*`; `backend/src/modules/scoring.cc`; migration `0011_scoring.sql`.
- **Evidence resolution:** each minute of the local day belongs to the highest-precedence evidence covering it
  (activities, skill sessions and consumed entertainment slots count as user; detections need confidence ≥ 0.4 and
  status detected/confirmed). Time blocks, minute targets, preemptions and per-area minutes all read that one map.
- **Planned items:** from `calendar.view` of the day. Recurring tasks filtered by their day profiles; an occurrence
  skipped before the day began is replanning (excluded), after it a miss. Tasks and deliverables completed within
  24 h after the close count 0.5. Weights: priority 0/1/2/3 → 1/1.5/2/3, blocks 1 per hour (min 0.5), deliverables 2.
- **Profiles:** override row, then a non-recurring all-day event `day_profile.<name>`, then `users.workdays`.
  Built-ins (workday, weekend, holiday scored; leave, sick not) unless a `day_profiles` row redefines the name.
  Configuration (profiles, overrides, targets, work contexts) is per tenant; scores are per user.
- **Status values:** `final`, `provisional` (base only while the assistant is offline; a `scores.close_day` with
  `force_adjust` is queued for the reset), `unobserved`, and `not_scored` (leave/sick, or nothing planned and no
  target applied).
- **Jobs:** `day.close` queues `scores.close_day` for the day and again for the day before (late entries), and
  `scores.close_period` on Sundays and month ends. Unchanged inputs hash → no new version; a re-score reuses the
  adjustment when the base moved ≤ 5 points; a dispute forces one re-run with the reason in the input.
- **Adjustment input:** breakdown, consistency, preemptions, active goals and plans, the day's summaries, work
  contexts, **logged activities and evidence minutes** (added after the live check: without them the model said
  "no work was done" about an unplanned 8-hour goal-critical day), and the dispute reason. The schema requires a
  reason for any non-zero adjustment (if/else), checked again in code.
- **Weekly targets** use per-area evidence minutes summed from the day breakdowns (category and link matches are
  not applied at week level yet); other metrics are counted from the records of the week.
- **Not yet:** dispute statistics and flags (§7), the intraday entertainment warning (§6.1, with triggers), the
  unobserved-day question (§4.6, with triggers), year scores, formula re-application jobs.
- **Open question (from the live check):** with `final = base · (1 + pct/100)` a day whose base is 0 stays 0 even at
  +30 %; the live model gave +25 % to an unplanned 8-hour goal-critical day that still showed 0.

## 13. Decisions (review round 4, 2026-11-11)

1. α / β = **0.8 / 0.2**. Plan adherence (staying in the flow) dominates; quantity still counts.
2. Over-achievement earns a bonus of up to +5 points per area; the area is capped at 100.
3. A day with no evidence is **unobserved**: excluded from averages, and the agent asks for context the next day (§4.6).
4. Work uses **both** a default 6 h workday target and planned work items (§5).
5. Weekly targets make up 20 % of the week score.
6. A detection needs confidence ≥ 0.4 to count as evidence.
