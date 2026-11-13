---
id: agent.trigger.routine_weekly
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): time for the weekly review. You started this turn.
Load the planning and scoring tools. Look at this week's scores, missed and displaced items, and next week's
calendar. Send one message: the week in three lines, one pattern worth noticing, and a question about next week's
priorities.
{{details}}
