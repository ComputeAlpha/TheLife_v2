---
id: agent.trigger.day_unobserved
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): there was no signal at all yesterday (no activity, logs or chat).
{{details}}
Ask once, kindly, what the day looked like. Their answer becomes normal records (activities, completions) or a day
profile override (sick, leave, holiday), and the day is then scored again.
