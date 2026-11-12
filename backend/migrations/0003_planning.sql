-- migrate:up
-- Planning resources (api/resources/goals.yaml, plans.yaml) and the idempotency store used by the REST adapter.
-- Column names match contract field names exactly: the generic CRUD layer maps fields to columns 1:1 and the
-- isolation suite checks every bound resource against the table.

CREATE DOMAIN area AS text CHECK (VALUE IN ('work', 'skill', 'fitness', 'personal', 'entertainment', 'other'));

CREATE TABLE goals (
  id                uuid PRIMARY KEY,
  tenant_id         uuid NOT NULL REFERENCES tenants(id),
  created_at        timestamptz NOT NULL DEFAULT now(),
  updated_at        timestamptz NOT NULL DEFAULT now(),
  created_by_kind   principal_kind NOT NULL,
  created_by_id     uuid,
  origin            origin_kind NOT NULL DEFAULT 'direct',
  source_message_id uuid,
  version           integer NOT NULL DEFAULT 1,
  deleted_at        timestamptz,
  title             text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  description       text CHECK (length(description) <= 10000),
  area              area NOT NULL,
  horizon           text NOT NULL CHECK (horizon IN ('year', 'quarter', 'month', 'week')),
  parent_id         uuid,
  target            jsonb,
  starts_on         date,
  due_on            date,
  status            text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'achieved', 'abandoned', 'paused')),
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, parent_id) REFERENCES goals (tenant_id, id),
  CHECK (parent_id IS DISTINCT FROM id),
  CHECK (due_on IS NULL OR starts_on IS NULL OR due_on >= starts_on)
);
SELECT app.setup_tenant_table('goals', true);
CREATE INDEX goals_tenant_idx ON goals (tenant_id, id) WHERE deleted_at IS NULL;
CREATE INDEX goals_updated_idx ON goals (tenant_id, updated_at);

CREATE TABLE plans (
  id                uuid PRIMARY KEY,
  tenant_id         uuid NOT NULL REFERENCES tenants(id),
  created_at        timestamptz NOT NULL DEFAULT now(),
  updated_at        timestamptz NOT NULL DEFAULT now(),
  created_by_kind   principal_kind NOT NULL,
  created_by_id     uuid,
  origin            origin_kind NOT NULL DEFAULT 'direct',
  source_message_id uuid,
  version           integer NOT NULL DEFAULT 1,
  deleted_at        timestamptz,
  goal_id           uuid,
  title             text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  body              text CHECK (length(body) <= 10000),
  period_start      date NOT NULL,
  period_end        date NOT NULL,
  status            text NOT NULL DEFAULT 'active' CHECK (status IN ('draft', 'active', 'done', 'abandoned')),
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, goal_id) REFERENCES goals (tenant_id, id),
  CHECK (period_end >= period_start)
);
SELECT app.setup_tenant_table('plans', true);
CREATE INDEX plans_tenant_idx ON plans (tenant_id, id) WHERE deleted_at IS NULL;
CREATE INDEX plans_updated_idx ON plans (tenant_id, updated_at);

-- Idempotency-Key store (backend_lld.md §7). The row is inserted before the handler runs, inside the same
-- transaction: a concurrent duplicate blocks on the unique index until the first commits, then replays it.
CREATE TABLE idempotency_keys (
  tenant_id     uuid NOT NULL REFERENCES tenants(id),
  principal_id  uuid NOT NULL,
  key           text NOT NULL CHECK (length(key) BETWEEN 8 AND 128),
  request_hash  bytea NOT NULL,
  status_code   integer,
  response      jsonb,
  created_at    timestamptz NOT NULL DEFAULT now(),
  updated_at    timestamptz NOT NULL DEFAULT now(),
  version       integer NOT NULL DEFAULT 1,
  PRIMARY KEY (tenant_id, principal_id, key)
);
SELECT app.setup_tenant_table('idempotency_keys', false);
GRANT DELETE ON idempotency_keys TO thelife_app;   -- 24 h garbage collection

-- migrate:down
DROP TABLE idempotency_keys;
DROP TABLE plans;
DROP TABLE goals;
DROP DOMAIN area;
