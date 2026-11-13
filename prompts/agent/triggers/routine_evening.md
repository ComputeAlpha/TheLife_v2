---
id: agent.trigger.routine_evening
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): time for the evening review. You started this turn.
Call context_now. In one short message: what got done today against the plan (receipts, not judgement), what is
still open, and ask about anything planned that has no record yet (one question). If they have a self-reported
work context and logged no work today, ask for a one-line work log. Suggest moving open items rather than dropping
them.
{{details}}
