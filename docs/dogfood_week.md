# Dogfood week (Wed 18 – Fri 20 Nov 2026)

Goal: every layer works on the always-on deployment (`deploy/README.md`) and the layers work together, before
daily use starts on Monday 23 Nov. After that there are no new features this year, only fixes.

Everything runs against `http://localhost:8090`, not the dev demo. For each problem, note the time, what you
did, what you expected and what happened. Logs: `scripts/prod.sh logs backend`,
`journalctl --user -u thelife-collector -u thelife-embedder`. Fixes ship with `scripts/prod.sh update`.

## Before Wednesday (setup)
- [ ] Telegram bot token in `~/.config/thelife/server.env`, then `scripts/prod.sh up`
- [ ] `scripts/prod.sh account …`, sign in, set the time zone and day start in Settings
- [ ] Telegram linked (Settings → Connections)
- [ ] Collector enrolled, `scripts/prod.sh install-collector`, and `thelife-collector status` says reachable
- [ ] `scripts/prod.sh status`: everything green; `server.env` and `kek` copied off the machine
- [ ] Dev demo (`scripts/demo.sh up`) stopped

## Wednesday: plan and capture (web, Telegram, agent)
- [ ] Plan: a goal, a plan under it, targets; a day profile and the area weights
- [ ] Tasks: one-off, recurring (weekly on two days), with a due time today; complete and skip an occurrence
- [ ] Calendar: events by drag and by form, a recurring block; week/month/year views agree
- [ ] Skills: a book and a course with sections; log a session from the web
- [ ] Work: role, responsibility with cadence, project with deliverables
- [ ] Entertainment: a game, plan a slot, mark it enjoyed
- [ ] Chat (web): "add a task …", "log 30 min of reading …", "what's on today?"; the steps show the tool calls
- [ ] Chat asks to delete or edit something you wrote → an approval appears (side panel slides open) → approve once, reject once
- [ ] Telegram: the same captures from the phone; they appear on the web without a reload
- [ ] Web chat ↔ Telegram: each side sees the other's day in its digest, not the full transcript
- [ ] Memory: tell it a preference; next day it is recalled; "Forget" works after confirmation

## Thursday: the collector loop (collector, vision, detections, scoring)
- [ ] A normal working morning with the collector on: Activity shows process detections within minutes
- [ ] Vision detections arrive at the scheduled interval (check the status pill's usage); none while the screen is locked
- [ ] `thelife-collector pause 30m` → nothing recorded in that window; `resume`
- [ ] Correct a wrong detection; dispute one; add a missed block with "Add"
- [ ] An app rule in Settings → Tracking changes how new windows are classified
- [ ] Scores for the day move with confirmed and corrected time; the day's breakdown makes sense
- [ ] Desktop notifications (approval, reminder) open the right page when clicked
- [ ] Restart the machine at the end of the day: containers come back by themselves; after login, embedder and collector too

## Friday: the proactive side (jobs, reminders, routines, memory)
- [ ] Reminders: one in 10 minutes (web) and one from Telegram; both arrive on the channel you chose, once
- [ ] Routines: morning or evening routine fires; "skip today" works; quiet hours are respected
- [ ] Day close: yesterday's summary exists; this morning's chat knows what happened yesterday
- [ ] Search (Ctrl K "Find") and chat recall find older items by meaning, not only by exact words (embedder)
- [ ] Weekly view of scores; the target progress on Plan matches what you logged
- [ ] `scripts/prod.sh backup` and check that the daily backup file exists
- [ ] Fix-forward rehearsal: `scripts/prod.sh update` with no change, and nothing is lost

## Bugs found

| # | Day | Layer | What happened | Status |
|---|---|---|---|---|
| 1 | Wed 25 Nov | agent / LLM gateway | A long, many-part message made every chat and Telegram turn fail with `invalid_output`: the model spent the whole 2048-token budget reasoning (`finish_reason: length`, no text, no tool call), and the retry sent the same request. Fix: agent budget 8192; a reply that runs out while reasoning is retried with `reasoning_effort=low`; final LLM failures are logged | fixed, to deploy |
| 2 | Wed 25 Nov | deploy / backup | No backup since 21 Nov: after a reboot the backup container starts before Postgres (Docker's restart ignores `depends_on`), the dump fails and it sleeps 24 h. Fix: wait for `pg_isready`, retry a failed dump in 5 min | fixed, to deploy |
