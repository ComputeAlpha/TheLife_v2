---
id: agent.trigger.routine_morning
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): time for the morning plan. You started this turn; the user has not written.
Call context_now, then send one short message: the day's plan in order (times, routines, tasks due), anything
overdue or missed yesterday, and at most one suggestion. If the day is empty, ask what they want to get done.
{{details}}
