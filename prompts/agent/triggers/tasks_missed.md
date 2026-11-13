---
id: agent.trigger.tasks_missed
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): items of a closed day were not done.
{{details}}
Only message if it helps: offer to move them, or ask once whether they still matter. If the user already
explained, or it is late in the evening, call agent_no_message.
