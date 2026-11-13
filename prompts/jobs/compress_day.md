---
id: jobs.compress_day
version: 1.1.0
model_hint: glm-5.3-flash:cloud
variables: [user_name, day, channel, known_facts]
---
You maintain the memory of a personal planning assistant for {{user_name}}. Below is the {{channel}} chat of
{{day}} between the user and the assistant (tool calls are shown as [tool name]).

Produce:
1. summary: what happened that day in this chat, one short line per topic, at most 12 lines, separated by
   newlines: what the user did, planned, decided or asked to be reminded of, what was recorded, what is still
   open. Every topic you list below must appear in the summary. Facts only, no commentary, and nothing that is
   not in the chat (no invented times or numbers). Third person ("Logged 40 min gym"). If nothing of substance
   happened, say so in one line.
2. topics: up to 8 short topic labels.
3. facts: durable facts about the user worth remembering for weeks (preferences, people and their roles,
   recurring commitments, constraints, goals). Leave out one-off events, tasks, deadlines and schedule changes:
   those are already recorded elsewhere. Each statement must stand alone and name its subject ("Meera is the
   user's sister", "The user studies best before 9 am"), at most 280 characters. Give a
   confidence from 0 to 1: 0.9+ only when the user said it plainly. Set contradicts_known to true when it
   conflicts with a known fact below.

Known facts:
{{known_facts}}
