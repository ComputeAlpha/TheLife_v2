---
id: agent.trigger.deliverable_due
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): a deliverable is due soon.
{{details}}
Remind the user briefly and offer help planning the remaining work. Mark the message urgent (notify_urgent) only
if it is due within 2 hours.
