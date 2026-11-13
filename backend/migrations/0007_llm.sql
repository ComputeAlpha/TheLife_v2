-- migrate:up
-- LLM gateway (agent_lld.md §3): provider state, per-call log and per-day usage counters.

-- Operational state that belongs to the deployment, not to a tenant. Kept out of `public` so the RLS catalog
-- rule (every public table is tenant-scoped) stays absolute.
CREATE SCHEMA ops AUTHORIZATION thelife_owner;
GRANT USAGE ON SCHEMA ops TO thelife_app;

-- Provider state machine (§3.4); survives restarts. One row per configured provider.
CREATE TABLE ops.provider_state (
  provider     text PRIMARY KEY CHECK (length(provider) BETWEEN 1 AND 64),
  state        text NOT NULL DEFAULT 'healthy'
               CHECK (state IN ('healthy', 'throttled', 'exhausted', 'probing', 'disabled')),
  until_at     timestamptz,           -- throttled: retry after; exhausted: resets_at estimate
  quota_scope  text CHECK (quota_scope IN ('hourly', 'session', 'daily', 'weekly', 'unknown')),
  reason       text,                  -- short classifier output, never request content
  changed_at   timestamptz NOT NULL DEFAULT now(),
  CHECK (state NOT IN ('throttled', 'exhausted') OR until_at IS NOT NULL)
);
GRANT SELECT, INSERT, UPDATE ON ops.provider_state TO thelife_app;

-- One row per provider call. Metadata only: no prompt or response content (§3.6).
CREATE TABLE llm_calls (
  id              uuid PRIMARY KEY,
  tenant_id       uuid NOT NULL REFERENCES tenants(id),
  purpose         text NOT NULL CHECK (purpose ~ '^[a-z0-9_.]{1,64}$'),
  provider        text NOT NULL,
  model           text NOT NULL,
  prompt_id       text,
  prompt_version  text,
  contract_version text,
  priority        text NOT NULL CHECK (priority IN ('interactive', 'proactive', 'vision', 'background')),
  conversation_id uuid,
  tokens_in       integer NOT NULL DEFAULT 0 CHECK (tokens_in >= 0),
  tokens_out      integer NOT NULL DEFAULT 0 CHECK (tokens_out >= 0),
  images          integer NOT NULL DEFAULT 0 CHECK (images >= 0),
  attempts        integer NOT NULL DEFAULT 1 CHECK (attempts >= 1),
  latency_ms      integer NOT NULL CHECK (latency_ms >= 0),
  status          text NOT NULL CHECK (status IN ('ok', 'error')),
  error_class     text,
  created_at      timestamptz NOT NULL DEFAULT now(),
  CHECK ((status = 'ok') = (error_class IS NULL))
);
CREATE INDEX llm_calls_tenant_created ON llm_calls (tenant_id, created_at DESC);
ALTER TABLE llm_calls ENABLE ROW LEVEL SECURITY;
ALTER TABLE llm_calls FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON llm_calls USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());
REVOKE UPDATE ON llm_calls FROM thelife_app;   -- append-only

-- Daily counters per tenant and purpose (§3.5), for "this week: N requests" and the soft warning.
CREATE TABLE llm_usage (
  tenant_id   uuid NOT NULL REFERENCES tenants(id),
  day         date NOT NULL,
  purpose     text NOT NULL,
  requests    integer NOT NULL DEFAULT 0,
  failures    integer NOT NULL DEFAULT 0,
  tokens_in   bigint NOT NULL DEFAULT 0,
  tokens_out  bigint NOT NULL DEFAULT 0,
  images      integer NOT NULL DEFAULT 0,
  updated_at  timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, day, purpose)
);
ALTER TABLE llm_usage ENABLE ROW LEVEL SECURITY;
ALTER TABLE llm_usage FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON llm_usage USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());

-- migrate:down
DROP TABLE llm_usage;
DROP TABLE llm_calls;
DROP TABLE ops.provider_state;
DROP SCHEMA ops;
