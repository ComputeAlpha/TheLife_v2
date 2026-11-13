---
id: jobs.rollup
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [user_name, period, period_start]
---
Combine these summaries of {{user_name}}'s {{period}} starting {{period_start}} into one summary of at most
10 short lines: main activities and progress, decisions, recurring problems, what is still open. Facts only,
third person, no commentary. The summaries follow in the next message, one per day.
