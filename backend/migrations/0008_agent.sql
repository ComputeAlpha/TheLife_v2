-- migrate:up
-- Agent core (agent_lld.md §5–7, §10): conversations, messages, turns, approvals, and the agent_actions audit.

-- One conversation per user, channel and local day (§1). Resource columns so the generic list/get path serves it.
CREATE TABLE conversations (
  id                uuid PRIMARY KEY,
  tenant_id         uuid NOT NULL REFERENCES tenants(id),
  created_at        timestamptz NOT NULL DEFAULT now(),
  updated_at        timestamptz NOT NULL DEFAULT now(),
  version           integer NOT NULL DEFAULT 1,
  created_by_kind   principal_kind NOT NULL DEFAULT 'user',
  created_by_id     uuid,
  origin            origin_kind NOT NULL DEFAULT 'direct',
  source_message_id uuid,
  deleted_at        timestamptz,
  user_id           uuid NOT NULL REFERENCES users(id),
  channel           text NOT NULL CHECK (channel IN ('web', 'telegram')),
  local_day         date NOT NULL,
  status            text NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'closed')),
  closed_at         timestamptz,
  UNIQUE (tenant_id, user_id, channel, local_day)
);
SELECT app.setup_tenant_table('conversations', true);

-- Append-only transcript. Tool results are stored as role=tool messages, linked by tool_call_id.
CREATE TABLE messages (
  id               uuid PRIMARY KEY,
  tenant_id        uuid NOT NULL REFERENCES tenants(id),
  conversation_id  uuid NOT NULL REFERENCES conversations(id),
  turn_id          uuid,
  role             text NOT NULL CHECK (role IN ('user', 'assistant', 'tool')),
  kind             text NOT NULL DEFAULT 'normal'
                   CHECK (kind IN ('normal', 'deterministic', 'suppressed', 'error', 'rolling_summary')),
  content          text NOT NULL DEFAULT '' CHECK (length(content) <= 200000),
  tool_calls       jsonb NOT NULL DEFAULT '[]'::jsonb,
  tool_call_id     text,
  tool_name        text,
  client_msg_id    text,
  prompt_id        text,
  prompt_version   text,
  contract_version text,
  tokens_in        integer,
  tokens_out       integer,
  created_at       timestamptz NOT NULL DEFAULT clock_timestamp(),  -- several messages share one transaction
  CHECK (role <> 'tool' OR tool_call_id IS NOT NULL)
);
CREATE INDEX messages_conversation ON messages (conversation_id, id);
CREATE UNIQUE INDEX messages_client_msg ON messages (tenant_id, conversation_id, client_msg_id)
  WHERE client_msg_id IS NOT NULL;
ALTER TABLE messages ENABLE ROW LEVEL SECURITY;
ALTER TABLE messages FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON messages USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());
REVOKE UPDATE ON messages FROM thelife_app;

-- One agent turn (§4–5): what started it and how it ended.
CREATE TABLE turns (
  id              uuid PRIMARY KEY,
  tenant_id       uuid NOT NULL REFERENCES tenants(id),
  user_id         uuid NOT NULL REFERENCES users(id),
  conversation_id uuid NOT NULL REFERENCES conversations(id),
  message_id      uuid REFERENCES messages(id),
  trigger         text NOT NULL CHECK (trigger IN ('user_message', 'approval')),
  status          text NOT NULL DEFAULT 'queued'
                  CHECK (status IN ('queued', 'running', 'done', 'failed', 'manual', 'merged')),
  error           text,
  steps           integer NOT NULL DEFAULT 0,
  tool_calls      integer NOT NULL DEFAULT 0,
  seen_until      timestamptz,  -- newest message in the context of this turn's last model call (§4.5 merging)
  created_at      timestamptz NOT NULL DEFAULT now(),
  updated_at      timestamptz NOT NULL DEFAULT now(),
  started_at      timestamptz,
  finished_at     timestamptz
);
CREATE INDEX turns_conversation ON turns (conversation_id, created_at);
ALTER TABLE turns ENABLE ROW LEVEL SECURITY;
ALTER TABLE turns FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON turns USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());
ALTER TABLE messages ADD CONSTRAINT messages_turn FOREIGN KEY (turn_id) REFERENCES turns(id) DEFERRABLE INITIALLY DEFERRED;

-- Held agent actions (spec 5.3, §5). Status changes only through the approve/reject path.
CREATE TABLE approval_requests (
  id                uuid PRIMARY KEY,
  tenant_id         uuid NOT NULL REFERENCES tenants(id),
  created_at        timestamptz NOT NULL DEFAULT now(),
  updated_at        timestamptz NOT NULL DEFAULT now(),
  version           integer NOT NULL DEFAULT 1,
  created_by_kind   principal_kind NOT NULL DEFAULT 'agent',
  created_by_id     uuid,
  origin            origin_kind NOT NULL DEFAULT 'direct',
  source_message_id uuid,
  deleted_at        timestamptz,
  user_id           uuid NOT NULL REFERENCES users(id),
  conversation_id   uuid REFERENCES conversations(id),
  turn_id           uuid REFERENCES turns(id),
  tool              text NOT NULL,
  args              jsonb NOT NULL,
  summary           text NOT NULL CHECK (length(summary) BETWEEN 1 AND 2000),
  status            text NOT NULL DEFAULT 'pending'
                    CHECK (status IN ('pending', 'approved', 'rejected', 'expired', 'stale', 'failed')),
  target_type       text,
  target_id         uuid,
  target_version    integer,
  expires_at        timestamptz NOT NULL DEFAULT now() + interval '72 hours',
  decided_at        timestamptz,
  decided_by        uuid,
  decision_reason   text CHECK (length(decision_reason) <= 500),
  result            jsonb,
  error             text,
  CHECK ((status = 'pending') = (decided_at IS NULL))
);
CREATE INDEX approval_requests_pending ON approval_requests (tenant_id, user_id) WHERE status = 'pending';
SELECT app.setup_tenant_table('approval_requests', true);
REVOKE UPDATE ON approval_requests FROM thelife_app;
GRANT UPDATE (status, decided_at, decided_by, decision_reason, result, error) ON approval_requests TO thelife_app;

-- The API's view: a pending approval past expires_at reads as expired (no job needed to flip it).
CREATE VIEW approvals WITH (security_invoker = true, security_barrier = true) AS
SELECT a.id, a.tenant_id, a.created_at, a.updated_at, a.version, a.created_by_kind, a.created_by_id, a.origin,
       a.source_message_id, a.deleted_at, a.user_id, a.conversation_id, a.turn_id, a.tool, a.args, a.summary,
       CASE WHEN a.status = 'pending' AND a.expires_at <= now() THEN 'expired' ELSE a.status END AS status,
       a.target_type, a.target_id, a.target_version, a.expires_at,
       CASE WHEN a.status = 'pending' AND a.expires_at <= now() THEN a.expires_at ELSE a.decided_at END AS decided_at,
       a.decision_reason, a.result, a.error
  FROM approval_requests a;
REVOKE INSERT, UPDATE, DELETE ON approvals FROM thelife_app;
GRANT SELECT ON approvals TO thelife_app;

-- Audit of every agent tool call (§5): what was asked, what the policy decided, what happened.
CREATE TABLE agent_actions (
  id               uuid PRIMARY KEY,
  tenant_id        uuid NOT NULL REFERENCES tenants(id),
  user_id          uuid NOT NULL REFERENCES users(id),
  conversation_id  uuid REFERENCES conversations(id),
  turn_id          uuid REFERENCES turns(id),
  message_id       uuid,             -- the assistant message that carried the call
  tool             text NOT NULL,
  args             jsonb NOT NULL,
  decision         text NOT NULL CHECK (decision IN ('allow', 'approval', 'deny', 'invalid', 'error', 'approved')),
  outcome          text NOT NULL CHECK (outcome IN ('executed', 'pending', 'denied', 'failed')),
  approval_id      uuid REFERENCES approval_requests(id),
  result_summary   text CHECK (length(result_summary) <= 1000),
  error            text CHECK (length(error) <= 1000),
  contract_version text NOT NULL,
  created_at       timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX agent_actions_turn ON agent_actions (turn_id);
ALTER TABLE agent_actions ENABLE ROW LEVEL SECURITY;
ALTER TABLE agent_actions FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON agent_actions USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());
REVOKE UPDATE ON agent_actions FROM thelife_app;

-- Notifications (§8). The Notifier fills these (Stage 2, later increment); the collector long-polls desktop ones.
CREATE TABLE notifications (
  id              uuid PRIMARY KEY,
  tenant_id       uuid NOT NULL REFERENCES tenants(id),
  user_id         uuid NOT NULL REFERENCES users(id),
  kind            text NOT NULL CHECK (kind ~ '^[a-z0-9_.]{1,64}$'),
  urgency         text NOT NULL DEFAULT 'normal' CHECK (urgency IN ('low', 'normal', 'critical')),
  title           text NOT NULL CHECK (length(title) BETWEEN 1 AND 80),
  body            text NOT NULL CHECK (length(body) <= 400),
  action          text CHECK (action IN ('open_chat', 'open_approval')),
  ref_type        text,
  ref_id          uuid,
  conversation_id uuid REFERENCES conversations(id),
  message_id      uuid,
  created_at      timestamptz NOT NULL DEFAULT now(),
  expires_at      timestamptz,
  resolved_at     timestamptz
);
CREATE INDEX notifications_open ON notifications (tenant_id, user_id, created_at) WHERE resolved_at IS NULL;
ALTER TABLE notifications ENABLE ROW LEVEL SECURITY;
ALTER TABLE notifications FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON notifications USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());

CREATE TABLE notification_deliveries (
  id              uuid PRIMARY KEY,
  tenant_id       uuid NOT NULL REFERENCES tenants(id),
  notification_id uuid NOT NULL REFERENCES notifications(id),
  channel         text NOT NULL CHECK (channel IN ('desktop', 'browser', 'telegram')),
  device_id       uuid REFERENCES devices(id),
  status          text NOT NULL CHECK (status IN ('queued', 'sent', 'shown', 'clicked', 'failed', 'skipped')),
  at              timestamptz NOT NULL DEFAULT now(),
  UNIQUE NULLS NOT DISTINCT (notification_id, channel, device_id)
);
ALTER TABLE notification_deliveries ENABLE ROW LEVEL SECURITY;
ALTER TABLE notification_deliveries FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON notification_deliveries USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());

-- migrate:down
DROP TABLE notification_deliveries;
DROP TABLE notifications;
DROP TABLE agent_actions;
DROP VIEW approvals;
DROP TABLE approval_requests;
ALTER TABLE messages DROP CONSTRAINT messages_turn;
DROP TABLE turns;
DROP TABLE messages;
DROP TABLE conversations;
