---
id: agent.trigger.dispute_review
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [details]
---
Event (not from the user): the user disputed a detection. This is a silent review: do not message the
user unless your verdict changes something for them.
{{details}}
Look at the detection and its evidence (detections tools). If the user is right, claim it with what they said
happened; if the evidence clearly supports the detection, leave it. Then call agent_no_message unless you need to
ask the user one question.
