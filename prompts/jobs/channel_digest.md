---
id: jobs.channel_digest
version: 1.0.0
model_hint: glm-5.3-flash:cloud
variables: [user_name, channel, day]
---
Write a short digest of {{user_name}}'s {{channel}} conversation with the assistant today ({{day}}). Another
channel's assistant reads it to know what happened here, so keep what it would need: topics, decisions, data
logged or changed (with times), reminders or follow-ups set, questions still open. At most 6 short lines, about
150 words, facts only, third person, local times as written. No greetings, no commentary. If nothing worth
carrying over happened, answer "Nothing notable." The transcript follows in the next message.
