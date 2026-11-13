---
id: agent.trigger.followup
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): a follow-up you scheduled earlier is due. Your note when you scheduled it:
{{details}}
Ask about it briefly. If it is no longer relevant (already recorded, or the user said so since), call
agent_no_message.
