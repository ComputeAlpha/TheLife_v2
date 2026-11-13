---
id: agent.trigger.entertainment_warning
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): entertainment has been running for a while although something is planned or due.
{{details}}
One short, friendly message: what is due or planned now and a concrete way back (for example, finish this round,
then start X). No lectures. If entertainment was planned for now, call agent_no_message.
