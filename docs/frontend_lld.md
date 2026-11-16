# TheLife v2: Frontend Low-Level Design (Stage 5)

Status: 5a done (2026-11-16): foundation, sign-in, shell, Settings → Profile and Security. Replaces the Flask demo UI (`prelim_spec.md` §8) with no loss of function.

User preferences (2026-11-16):
- One theme only: pure black (AMOLED) with a cyan accent. There is no theme setting.
- The UI is the final product. It should be expressive and complete, unlike the debugging-oriented demo UI.
- The calendar uses the usual layouts (day, week, month, year) with entries placed by date and time, similar to the Microsoft Teams calendar.

The rest of this document covers the choices made from those preferences and the existing contract.

---

## 1. Stack

| Concern | Choice | Why |
|---|---|---|
| Build | Vite 6, TypeScript (strict), React 19 | Standard, fast HMR, static output |
| Routing | React Router 7 (library mode) | Nested layouts and URL state for views and dates |
| Server state | TanStack Query 5 | Caching, invalidation from SSE events, optimistic updates |
| API types | `openapi-typescript` generated from `api/generated/openapi.yaml`, with `openapi-fetch` | The contract stays the single source; CI fails if the generated types drift |
| Styling | Tailwind CSS 4 with CSS-variable design tokens (§3) | Tokens in one place; no runtime CSS-in-JS (CSP stays strict) |
| Primitives | Radix UI (dialog, popover, dropdown, tooltip, tabs, toast, switch) | Accessible behaviour; styled entirely by us |
| Motion | `motion` (Framer Motion) | Layout and presence animations; honours `prefers-reduced-motion` |
| Icons | `lucide-react` | Consistent stroke icons |
| Dates | `date-fns` 4 with `@date-fns/tz` | All rendering is in the **user's** timezone from `/me`, not the browser's |
| Recurrence | `rrule` | Human-readable rule text ("every weekday at 07:00") and the rule builder |
| Chat text | `react-markdown` + `remark-gfm`, raw HTML disabled | Model output is never injected as HTML |
| Charts | Hand-written SVG components (rings, bars, sparklines, trend lines) | Few chart types, full control of the look, no large dependency |
| Fonts | Self-hosted through `@fontsource-variable` (Inter, Space Grotesk, JetBrains Mono) | No requests to font CDNs |
| Tests | Vitest + Testing Library + MSW; Playwright (+ `@axe-core/playwright`) | §9 |

The code lives in `frontend/`. npm is the package manager, and `package-lock.json` is committed. Node 22 LTS is required.

## 2. Hosting and Authentication

### 2.1 Same origin
The browser only talks to its own origin, so the backend needs no CORS.
- **Development:** the Vite dev server (`:5173`) proxies `/api` to the backend (`:8080`), including SSE.
- **Running:** the backend serves the built `frontend/dist` when `THELIFE_WEB_ROOT` is set.
  - Any path outside `/api`, `/healthz` and `/readyz` that has no matching file gets `index.html` (SPA fallback).
  - Hashed assets are cached as immutable. `index.html` is sent with `no-store`.
  - `scripts/demo.sh up` builds the frontend and sets the variable.

Headers on every web response:
- `Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'`
- `X-Content-Type-Options: nosniff`
- `Referrer-Policy: no-referrer`

Scripts load only from the origin. Styles also allow inline `<style>` elements, because the dialogs' scroll lock injects a generated stylesheet whose content varies (scrollbar width), so it can't be pinned by a hash. The build inlines no assets as `data:` URIs (`assetsInlineLimit: 0`).

### 2.2 Web session cookie (backend change)
The demo UI kept session tokens off the browser. The SPA keeps that property: its token sits in an HttpOnly cookie, never in JavaScript or storage.

**Login and logout**
- `POST /api/v1/auth/web-session {email, password}` returns `200 {expires_at, user}` and sets `tl_session=<tlu_ token>; HttpOnly; Secure; SameSite=Strict; Path=/api; Max-Age=<expiry>`.
  - It uses the same rate limits and credential row as `auth.login`, with the credential named `web session (browser)`.
  - Browsers accept `Secure` cookies on `http://127.0.0.1` and `localhost`.
- `DELETE /api/v1/auth/web-session` revokes the credential and clears the cookie. Logging out of the bearer API (`POST /auth/logout`) works too.

**How the authenticator treats the cookie**
- It reads the cookie only when there is no `Authorization` header.
- It accepts only `user_session` tokens from the cookie (not device tokens or API keys).

**CSRF:** a cookie-authenticated request with an unsafe method (POST, PATCH, PUT, DELETE) must carry `X-TheLife-CSRF: 1`, or it gets `403 csrf-required`.
- A custom header cannot be sent cross-site without a CORS preflight, which the backend never grants.
- This sits on top of `SameSite=Strict`.

**SSE:** `EventSource('/api/v1/chat/web/stream')` sends the cookie on its own, so streaming needs no token handling.

**Tests (backend `api/web_session_test.cc`):**
- login sets the cookie flags;
- cookie authentication works;
- a missing CSRF header on POST gets 403;
- a device token or API key in the cookie gets 401;
- logout revokes the credential and clears the cookie;
- a bearer request ignores the cookie;
- static serving and the SPA fallback work, with the headers above;
- `/api/*` paths that don't exist still return problem+json 404 (no fallback).

## 3. Design System

### 3.1 Colour tokens (single dark theme)
| Token | Value | Use |
|---|---|---|
| `--bg` | `#000000` | Page. Pure black for OLED |
| `--surface-1` | `#0a0b0d` | Cards, sidebar |
| `--surface-2` | `#111316` | Raised: popovers, sheets, inputs |
| `--surface-3` | `#181b20` | Hover, selected rows |
| `--line` | `#1f2329` | Hairlines and borders; depth comes from these, not from shadows |
| `--text` / `--text-2` / `--text-3` | `#f4f5f7` / `#a3a9b3` / `#6b7280` | Primary, secondary, muted |
| `--accent` | `#22d3ee` (cyan) | Primary actions, focus, selection, "now" line |
| `--accent-strong` / `--accent-soft` | `#67e8f9` / `rgb(34 211 238 / .12)` | Hover / tinted backgrounds |
| `--good` | `#34d399` (emerald) | Done, confirmed, positive adjustment |
| `--alert` | `#db2777` (dark pink), text `#f472b6` | Disputed, missed, destructive, negative adjustment |
| `--warn` | `#fbbf24` | Pending approval, provisional, offline assistant |

The spec's emerald and dark-pink accents are used only for meaning (good and bad). Cyan is the one brand accent.

**Area colours** are used on calendar chips, timeline blocks and progress bars, always paired with an icon so colour is not the only cue:

| Area | Colour | Hex |
|---|---|---|
| work | cyan | `#22d3ee` |
| skill | violet | `#a78bfa` |
| fitness | emerald | `#34d399` |
| personal | orange | `#fb923c` |
| entertainment | pink | `#f472b6` |
| other | slate | `#94a3b8` |

### 3.2 Type, spacing, motion
- **Type**
  - Body: Inter Variable. Display (headings, big numbers): Space Grotesk. Code, IDs and tool arguments: JetBrains Mono.
  - Numbers use `tabular-nums`.
- **Spacing and shape**
  - 4 px spacing grid.
  - Corner radius 10 px on cards and 8 px on controls; chips are pill-shaped.
- **Motion**
  - Durations: 120 ms (hover), 180 ms (popover), 240 ms (sheet and route).
  - Layout animation for list reorder and calendar view switches.
  - With `prefers-reduced-motion`, everything becomes a fade or nothing.
- **Expressive elements**
  - A soft cyan glow on focus and on the primary button.
  - Animated score rings.
  - Area-tinted gradients on progress bars.
  - The streaming caret in chat.
  - Line-art empty states.
  - Subtle confetti on a completed day: once per day, and off under reduced motion.
- **Accessibility**
  - Text contrast at least 4.5:1 on black. All the colours above pass against `--surface-1`.
  - Visible focus rings; full keyboard operation.
  - Radix ARIA patterns; an axe check in e2e.

### 3.3 Components (`src/ui/`)
- **Controls:** Button (primary, secondary, ghost, danger; sizes), IconButton, Input, Textarea, Select, Combobox (for references such as goal or project), DateTimeField (in the user's timezone), DurationField, RRuleBuilder, Switch, Checkbox, Chip/Tag, AreaBadge, StatusBadge, OriginBadge (direct, user_stated, inferred: "added by assistant").
- **Overlays:** Sheet (right-side editor), Dialog, Popover, Tooltip, Toast, ConfirmDialog, TokenOnceDialog (copy, then the token is gone).
- **Data display:** Card, StatRing, ProgressBar, Sparkline, TrendChart, EmptyState, Skeleton, ErrorState (problem+json aware, shows `request_id`), Kbd.
- **Resource editing:** `ResourceSheet` is built from typed field definitions per resource. It is not generated at runtime, but a test checks every field against the contract schema. It sends RFC 3339 times, merge-patch bodies with only the changed fields, `If-Match: "<version>"`, and an `Idempotency-Key` on creates.

## 4. Application Shell

- **Desktop (≥ 1024 px)**
  - Left sidebar, collapsible to icons:
    - Today, Calendar, Chat, Activity, Approvals (count badge)
    - Plan (Goals, Plans, Targets), Tasks, Skills, Work, Entertainment, Scores
    - Assistant (Routines, Reminders, Memory), Settings
  - Top bar: page title and date context, search and command palette trigger, assistant status pill, notifications bell, quick-add (+).
- **Tablet and phone:** a bottom bar (Today, Calendar, Chat, Activity, More); sheets become full-height.
- **Quick chat:** a floating "Ask" button on every page except Chat. It opens a chat side sheet over the current page, on the same conversation and stream.
- **Command palette (Ctrl/⌘ K):**
  - jump to any page;
  - create (task, event, goal, …);
  - search tasks, goals and resources by title;
  - "Ask the assistant: …" sends the text to chat.
- **Assistant status pill** (from `/system/llm-status`, polled every 60 s and on focus):
  - online: cyan dot;
  - manual: amber dot, with "Assistant offline until …" and the reason;
  - unconfigured: grey dot.
  - Clicking it opens provider and usage details.
- **Notifications:** the bell drawer lists `/notifications` (unresolved first) with ack. While a tab is open, SSE `notification` events show a toast. When the tab is hidden and the user has granted permission, they also show a browser `Notification`. Clicking one opens its action (chat, approval, task).
- **Keyboard shortcuts:**
  - `g t` today, `g c` calendar, `g a` activity, `g h` chat;
  - `n` new (in context), `?` shortcut sheet.
  - In the calendar: `t`, `d`, `w`, `m`, `y`, and the arrow keys.

## 5. Data Layer

- **`api/client.ts`:** `openapi-fetch` with the generated `paths`.
  - Middleware adds `X-TheLife-CSRF: 1` and `Idempotency-Key` (UUID) on creates.
  - It turns problem+json into a typed `ApiError` (status, title, detail, `errors[]` mapped onto form fields, `request_id`).
- **Query keys:** `[resource, params]`.
  - Lists page through `next_cursor` (infinite queries where a list can grow).
  - The calendar caches per window.
- **Errors by status**
  - `401`: clear the cache and go to `/login?next=…`.
  - `412` / `409 version-conflict` on an edit: the sheet shows "Changed elsewhere", with the server version next to the user's edits and "Keep mine" (retry with the new version) or "Use theirs".
  - `202 PendingDecision` (an edit held for approval, rare for the user principal): a toast linking to the approval.
- **The SSE stream** (`useEventStream`, one per tab, opened after login):
  - `turn.started`, `token`, `reset`, `tool.started`, `tool.finished`, `message`, `turn.finished` drive the chat reducer.
  - `tool.finished` also invalidates the query family of the tool's resource (for example `tasks.*` → `['tasks']`, `['calendar']`), so pages refresh live when the assistant changes data.
  - `approval.requested` invalidates `['approvals']` and adds the badge. `notification` triggers a toast and invalidates `['notifications']`.
  - The stream has no event ids. After a reconnect (backoff 1 s → 30 s), the client refetches the open conversation and the approvals.
- **Time:**
  - `useUser()` provides `timezone`, `day_start_hour` and `workdays`. Every date helper takes the zone explicitly.
  - A "local day" (`LocalDate`, used for scores, occurrences and overrides) respects `day_start_hour`. The calendar grid still runs 00:00–24:00 and shades the hours before the day start.

## 6. Screens

Each screen spec gives its purpose, layout, data, interactions and states. Every screen has loading skeletons, an empty state with a next action, and an error state with retry.

### 6.1 Login (`/login`)
- **Layout:** centred card on black, with a slow animated cyan gradient arc behind the wordmark. Fields: email and password. "Signed in for 30 days" note.
- **Data:** `POST /auth/web-session`.
- **States:**
  - `401`: "Email or password is wrong".
  - `429`: "Too many attempts, try again in N min" (from `Retry-After`).
- **After sign-in:** go to `next` (same-origin paths only) or Today.

### 6.2 Today (`/`)
The daily home: what's planned, how it's going, and what needs a decision.
- **Header:** greeting, local date, and the day-profile chip (workday, weekend, leave, …; click to override for today).
- **Row 1**
  - Day progress ring: plan adherence so far, from `/progress/today`.
  - Yesterday's score: final with ring and adjustment arrow; provisional or unobserved states shown.
  - Streaks: consistency items from the latest day score.
  - Collector status: last device heartbeat, from `/devices`.
- **Agenda** (`/calendar` for today):
  - a vertical list with a "now" divider; the next item is highlighted;
  - task occurrences have a check / skip control inline;
  - "Open in calendar".
- **Area progress:** per area, planned versus covered minutes and done versus planned items, as area-tinted bars. Targets show amount and progress.
- **Needs you**
  - Pending approvals (approve or reject inline).
  - Unreviewed detections from the last 3 hours (confirm or dispute inline).
  - Preemptions (`/insights/preemptions`): "Gaming overlapped *Read 20 pages* for 45 min".
- **Coming up:** the next reminders and routines (morning plan, evening review).
- **Interactions:** every card links to its full page, and quick-add is in the header.

### 6.3 Calendar (`/calendar/:view/:date`)
Plan and review time in a familiar calendar.

**Views**
- **Day:** one time-grid column.
- **Week:** 7 columns. **Work week:** the user's `workdays` only.
- **Month:** 6×7 grid.
- **Year:** 12 mini months.
- **Agenda:** a list grouped by day, 30 days.
- The view and date live in the URL. The view switch is a segmented control.

**Toolbar**
- Today; previous and next (by view); a date picker popover showing a mini month.
- View switch.
- Filters: kind (events, tasks, skill sessions, entertainment) and area chips.
- "Show activity" toggle.
- "+ New".

**Time grid (day and week)**
- 48 half-hour rows with hour labels and a sticky header of day columns.
- An all-day row on top, holding all-day events and day-profile overrides.
- A cyan "now" line with a dot. On open, the grid scrolls to an hour before now (or the day start).
- **Layout of overlaps:** entries are grouped into collision clusters, and each cluster splits the column width. This is a pure function, unit-tested.
- **Entry chip:** area-coloured left bar on a tinted fill, with title, time and kind icon.
  - Task occurrence: checkbox. Done: struck through with an emerald check. Skipped: dashed outline. Missed: pink outline.
  - Recurring entries show a ↻ icon.
- **Activity overlay** (toggle): detections for the window (`/detections?from&to`) are drawn as a narrow translucent lane on the right of each day column, so plan and reality sit side by side. Hovering one shows its summary; clicking opens it in Activity.

**Month**
- Day cells list up to 3 entry chips, then "+N more", which opens a day popover.
- Each cell shows a day-profile badge (leave, sick, holiday) and a small score dot coloured by final score (from `/scores?period=day`).
- Today's cell has a cyan ring. Weekends (non-workdays) are a step dimmer.

**Year**
- 12 mini months with per-day intensity (number of planned entries, or the day score; a toggle switches between them). Clicking a day opens its Day view.
- Data comes from six parallel 62-day `/calendar` windows and one `/scores` list.

**Agenda:** a list with day headers and the same chips. Infinite scroll by 30-day windows.

**Interactions**
- Clicking or dragging over empty grid time opens quick-create with the range filled in:
  - Event (`/events`)
  - Task with a due time (`/tasks`)
  - Skill session (`/skills/sessions`, pick a resource)
  - Entertainment slot (`/entertainment/slots`, pick an item)
  - "More options" opens the full sheet: notes, location, recurrence builder, area, category, link.
- Clicking an entry opens a popover with details and actions:
  - Event: edit, delete, duplicate.
  - Task occurrence: complete or skip (with a reason), open task.
  - Slot: mark consumed.
  - Skill session: open resource.
- Dragging or resizing an event, slot or skill session reschedules it: PATCH with `If-Match`, optimistic, reverted on failure.
- Recurring items show "Editing changes the whole series", because the contract has no per-occurrence overrides for events. Task occurrences are acted on per date.
- **Day-profile override:** right-click a day header (or use the day popover menu) to choose "Mark as leave / sick / holiday / …". This uses `/day-profile-overrides`.

**Data:** `/calendar?from&to` per visible window, prefetching the neighbouring window. If the response is `truncated`, a banner suggests a narrower view.

### 6.4 Chat (`/chat`, `/chat/:conversationId`)
- **Layout**
  - Left: history grouped by day, with channel badges (web, telegram). Telegram transcripts are read-only.
  - Centre: the conversation.
  - Right (wide screens only): a context panel with "Today at a glance" and pending approvals.
- **Messages**
  - User messages are right-aligned on `--surface-2`.
  - Assistant messages are full-width markdown with a streaming caret. On `reset`, the streamed text is replaced with a subtle shimmer.
  - Deterministic and suppressed messages are styled as system notes.
- **Tool activity:** compact chips between messages, for example "✓ Created task *Read 20 pages*" or "⟳ Looking up calendar…". Each expands to show arguments and result (mono, pretty JSON) with the timing.
- **Approvals inline:** `approval.requested` renders a card showing the tool, the summary, the exact arguments, the expiry countdown, and Approve or Reject (with an optional reason). The `#approval-<id>` anchor scrolls to it and highlights it; collector desktop notifications link there.
- **Composer**
  - Auto-growing textarea. Enter sends; Shift+Enter adds a newline.
  - Disabled with an explanation while the assistant is offline; deterministic commands still work, and "Commands" lists them.
  - Starter chips on an empty day: "Plan my day", "What did I do yesterday?", "Add a reading habit".
- **Delivery:** `client_msg_id` is a UUID kept until the turn is acknowledged, so a retry never duplicates a message. Turn states queued, running, failed and merged are shown on the user message.

### 6.5 Activity (`/activity/:date`)
Review what the collector and the assistant detected; confirm, dispute or correct it.
- **Header**
  - Day picker.
  - Summary bar: counted minutes per area, as a stacked horizontal bar. Counted means confirmed, plus detected with confidence ≥ 0.4, not disputed or superseded; this matches `area_minutes` in the demo UI.
  - Filters: source (vision, process, user, external), status, area.
- **Timeline:** a vertical hour axis with detection blocks sized by duration.
  - Each block: area colour, summary, category, a source icon (eye for vision, window for process, person for user), a confidence meter, and a status badge.
  - Superseded detections are hidden unless "Show corrections" is on; then the correction chain is drawn.
  - Planned calendar blocks appear as ghost outlines behind the detections.
- **Actions per detection**
  - **Confirm** (one tap).
  - **Dispute** (with an optional reason). The assistant re-evaluates it, and the card shows "Under review".
  - **Correct:** claim it with a new area, category, summary and time range. This creates a superseding detection.
  - **History:** a drawer with events (actor, from → to, reason) and the chain.
- **Bulk actions:** select several detections and confirm them all.
- **Tabs:** Detections | Activities. The Activities tab is the manual log (`/activities` CRUD), with "Log activity".

### 6.6 Approvals (`/approvals`)
- **Tabs:** Pending (default, newest first, expiry countdown) | History (approved, rejected, expired, stale, failed, with the decision reason, result and error).
- **Card:** the tool name in plain words (for example `tasks.update` → "Update task"), the summary, and the target link. Arguments are shown as a field list; for updates, they are shown against the target's current values when it can be fetched.
- **Actions:** approve, or reject with a reason. After approving, the result is shown. A stale approval explains that the target changed.

### 6.7 Scores (`/scores/:period`)
- **Period tabs:** day, week, month.
- **Trend chart:** final score (cyan line) and base (dim line) for the last 30 days, 12 weeks or 12 months. The dots are coloured by status: provisional and unobserved are hollow, not scored is grey.
- **Detail for the selected period**
  - A big ring with the final score; base plus the adjustment percentage, with the reasons listed (▲ emerald / ▼ pink, with references linked).
  - Breakdown per area: adherence and quantity bars, and items with weight and completion.
  - Targets met; consistency streaks; coverage (scored days out of calendar days).
  - Profile and formula version.
- **Actions:** dispute the score (with a reason), and view version history.

### 6.8 Plan: Goals, Plans and Targets (`/plan`)
- **Goals:** a tree by horizon (year → quarter → month → week) with status, due date and area. Linked tasks, plans, targets and skill resources are shown as counts.
- **Plans:** cards per period, linked to a goal, with a markdown body.
- **Targets:** a list with metric, amount and period, plus today's progress from `/progress/today` as a bar.
- Create and edit through sheets. A parent picker limits parents to a coarser horizon.

### 6.9 Tasks (`/tasks`)
- **Smart groups:** Today, Overdue, Upcoming, Recurring, No date, Done, with area, priority, goal and project filters.
- **Rows:** checkbox, title, area badge, priority marker, due date (relative), recurrence text, origin badge.
- **Detail sheet**
  - All fields; the recurrence builder (frequency, interval, days, end) with a preview of the next 5 dates; day profiles; track consistency.
  - An occurrence strip for the last and next 14 days, with complete or skip on each.

### 6.10 Skills (`/skills`)
- **Library grid:** book and course cards with a progress ring (from `progress`), author and status.
- **Resource page:** for a course, an ordered sections list with durations and done marks; a sessions log; "Log session" (page from/to for books, section and position for courses); and the linked goal.

### 6.11 Work (`/work`)
- **Roles:** cards, each with its responsibilities (cadence, active) and projects.
- **Project page:** deliverables on a due-date timeline with status, plus linked tasks.
- **Work contexts:** evidence mode, devices and workday target minutes. Kept in Settings → Work tracking, linked from here.

### 6.12 Entertainment (`/entertainment`)
- Items by kind (games, movies, series) with status chips (planned, playing, done …).
- Scheduled slots for the next 14 days, with "consumed".
- A note on the page: entertainment is planned, never limited.

### 6.13 Assistant (`/assistant/routines`, `/assistant/reminders`, `/assistant/memory`)
- **Routines:** cards with kind, mode (fixed or adaptive), window or default time, learned time, weekdays and profiles; an enable switch; "skip today"; the instructions text.
- **Reminders:** a list with due time and recurrence, channel preference, status and link.
- **Memory:** searchable facts.
  - Each fact: statement, confidence, status, when valid, last used; the superseded chain on expand.
  - Forget = delete, with a confirmation.

### 6.14 Settings (`/settings/*`)
- **Profile:** display name, timezone (searchable IANA list), day start hour, workdays.
- **Security:** password change.
- **Connections**
  - Telegram link: status, link code with a countdown and instructions, unlink.
  - Devices: list with last seen, version and status. Register a device: name and platform, then show the token once, with the enroll command. Rename and revoke.
  - API keys: create with scopes and expiry, then show the token once. Revoke.
- **Tracking:** ingest rules (match fields → app, category, area, confidence; enable switch) and work contexts.
- **Days and scoring:** day profiles and overrides; scoring weights per area and plan share, as sliders that sum visibly.
- **Notifications:** browser notification permission, and test.
- **Assistant:** LLM status, providers, usage today.
- **About:** versions.

## 7. Parity with the Demo UI

The exit gate is "no loss of function". It is checked mechanically:

- **Operation coverage:** `frontend/scripts/parity.ts` lists every operation in the contract whose principals include `user` and whose exposure includes `rest`, then checks that the frontend source calls each one through the typed client. Device-only operations (`ingest.*`, `devices.self.*`) and `auth.login` (the bearer form) are allow-listed with reasons. The check runs in CI.
- **Hand-written demo pages** (calendar, timeline, chat with approvals, settings, task occurrences, detection history, conversation transcript, token-once pages) each map to a screen above and to a Playwright test.
- **Retirement:** once the gate passes, `scripts/demo.sh` stops starting the Flask UI and `demo-ui/` moves to `spikes/` or is deleted. Which of the two is the user's call.

## 8. Project Layout

```
frontend/
  package.json, vite.config.ts, tsconfig.json, eslint.config.js, playwright.config.ts
  index.html
  src/
    main.tsx, App.tsx, routes.tsx
    api/        schema.d.ts (generated), client.ts, errors.ts, queries/<resource>.ts, stream.ts
    lib/        time.ts, rrule.ts, calendarLayout.ts, areas.ts, format.ts
    ui/         design-system components (§3.3), tokens.css
    shell/      Sidebar, TopBar, CommandPalette, QuickChat, NotificationsDrawer, StatusPill
    features/   today/, calendar/, chat/, activity/, approvals/, scores/, plan/, tasks/, skills/, work/,
                entertainment/, assistant/, settings/, auth/
  tests/        e2e/*.spec.ts (Playwright), fixtures
  scripts/      gen-api.mjs, parity.ts
```

## 9. Testing

- **Unit and component tests** (Vitest with jsdom and MSW):
  - calendar overlap layout, time-grid geometry, month and year matrices, timezone and day-start helpers;
  - rrule text and builder round trip; the SSE chat reducer (including `reset` and merged turns);
  - query invalidation on tool events; the problem+json → form-error mapping; If-Match conflict handling;
  - every feature's main component against MSW handlers typed from the contract.
- **End-to-end tests** (Playwright with Chromium) against the **real backend** on the test DB with `docker/fake-llm`, as the demo UI's `test_e2e_live.py` does:
  - login and logout;
  - creating, dragging and resizing an event in the week view;
  - completing a task occurrence;
  - chat streaming with a scripted tool call and an approval (approve and reject);
  - detection confirm, dispute and correct (with seeded detections);
  - device registration showing the token once; Telegram link code;
  - score detail.
  - Each test runs an axe scan.
- **Screen verification with Claude in Chrome** (the spec deliverable): after each increment, every new screen is walked through on the real backend, on desktop and phone widths.
- **CI:** a `frontend` job runs `npm ci`, lint, typecheck, the API types drift check, the parity check, Vitest and the build. The e2e tests join the backend job, which already has Postgres and the fake LLM.

## 10. Increments

| Inc | Scope | Done when |
|---|---|---|
| **5a** | Backend web session cookie, CSRF header and static serving (§2). Frontend scaffold, design tokens and components, API client and types, shell (sidebar, top bar, status pill, command palette skeleton), login, Settings → Profile and Security. `demo.sh up` serves it | Backend tests pass; frontend lint, types and unit tests pass; e2e login works |
| **5b** | Calendar (day, week, work week, month, year, agenda; create, drag, resize, popovers; activity overlay; day-profile overrides) and Today | Layout unit tests; e2e calendar flows; Chrome walkthrough |
| **5c** | SSE stream, Chat (history, streaming, tool chips, inline approvals), quick-chat sheet, Approvals page, notifications drawer, toasts and browser notifications | Reducer tests; e2e chat with the fake LLM; Chrome walkthrough |
| **5d** | Activity review (timeline, confirm, dispute, correct, history, activities log), Scores, Plan (goals, plans, targets) | e2e detection flows; Chrome walkthrough |
| **5e** | Tasks, Skills, Work, Entertainment, Assistant pages, the remaining Settings (Telegram, devices, API keys, ingest rules, day profiles, scoring, notifications, LLM status), parity check, demo UI retirement | Parity check green; all stage tests green; **exit gate** |
