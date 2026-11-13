-- migrate:up
-- Scoring (scoring_lld.md): configuration (day profiles, overrides, targets, work contexts, settings), append-only
-- score versions, preemptions and adjustment disputes. Configuration is per tenant (a tenant is one person's
-- account); scores carry the user.

CREATE TABLE day_profiles (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  name text NOT NULL CHECK (name ~ '^[a-z][a-z0-9_]{0,39}$'),
  scored boolean NOT NULL DEFAULT true,
  freezes_streaks boolean NOT NULL DEFAULT false,
  description text CHECK (length(description) <= 10000)
);
CREATE UNIQUE INDEX day_profiles_name ON day_profiles (tenant_id, name) WHERE deleted_at IS NULL;
SELECT app.setup_tenant_table('day_profiles', true);

CREATE TABLE day_profile_overrides (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  local_day date NOT NULL,
  profile text NOT NULL CHECK (profile ~ '^[a-z][a-z0-9_]{0,39}$'),
  note text CHECK (length(note) <= 200)
);
CREATE UNIQUE INDEX day_profile_overrides_day ON day_profile_overrides (tenant_id, local_day) WHERE deleted_at IS NULL;
SELECT app.setup_tenant_table('day_profile_overrides', true);

CREATE TABLE targets (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  title text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  area text NOT NULL CHECK (area IN ('work', 'skill', 'fitness', 'personal', 'entertainment', 'other')),
  metric text NOT NULL CHECK (metric IN ('minutes', 'pages', 'sections', 'sessions', 'count')),
  amount numeric NOT NULL CHECK (amount > 0 AND amount <= 100000),
  period text NOT NULL DEFAULT 'day' CHECK (period IN ('day', 'week')),
  day_profiles text[] NOT NULL DEFAULT '{}',
  goal_id uuid,
  match jsonb NOT NULL DEFAULT '{}'::jsonb,
  track_consistency boolean NOT NULL DEFAULT false,
  active_from date, active_to date,
  CHECK (active_to IS NULL OR active_from IS NULL OR active_to >= active_from)
);
SELECT app.setup_tenant_table('targets', true);

CREATE TABLE work_contexts (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  name text NOT NULL CHECK (length(name) BETWEEN 1 AND 200),
  employer text CHECK (length(employer) <= 200),
  evidence_mode text NOT NULL CHECK (evidence_mode IN ('observed', 'self_reported')),
  device_ids uuid[] NOT NULL DEFAULT '{}',
  workday_target_min integer NOT NULL DEFAULT 360 CHECK (workday_target_min BETWEEN 0 AND 1440),
  active_from date, active_to date,
  CHECK (active_to IS NULL OR active_from IS NULL OR active_to >= active_from)
);
SELECT app.setup_tenant_table('work_contexts', true);

CREATE TABLE scoring_settings (
  tenant_id  uuid NOT NULL REFERENCES tenants(id),
  user_id    uuid PRIMARY KEY REFERENCES users(id),
  weights    jsonb NOT NULL DEFAULT '{}'::jsonb,
  plan_share jsonb NOT NULL DEFAULT '{}'::jsonb,
  updated_at timestamptz NOT NULL DEFAULT now()
);
ALTER TABLE scoring_settings ENABLE ROW LEVEL SECURITY;
ALTER TABLE scoring_settings FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON scoring_settings USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());

-- Append-only versions (§9): a recompute adds a row and moves is_current.
CREATE TABLE score_versions (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL DEFAULT 'system', created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  user_id         uuid NOT NULL REFERENCES users(id),
  period          text NOT NULL CHECK (period IN ('day', 'week', 'month')),
  period_start    date NOT NULL,
  profile         text,
  formula_version text NOT NULL,
  inputs_hash     text NOT NULL,
  breakdown       jsonb NOT NULL DEFAULT '{}'::jsonb,
  consistency     jsonb NOT NULL DEFAULT '{}'::jsonb,
  coverage        jsonb,
  base            numeric(5,2) CHECK (base BETWEEN 0 AND 100),
  adjustment_pct  numeric(4,1) CHECK (adjustment_pct BETWEEN -30 AND 30),
  adjustment      jsonb,
  final           numeric(5,2) CHECK (final BETWEEN 0 AND 100),
  status          text NOT NULL CHECK (status IN ('provisional', 'final', 'unobserved', 'not_scored')),
  is_current      boolean NOT NULL DEFAULT true
);
CREATE UNIQUE INDEX score_versions_current ON score_versions (tenant_id, user_id, period, period_start) WHERE is_current;
CREATE INDEX score_versions_period ON score_versions (tenant_id, user_id, period, period_start);
SELECT app.setup_tenant_table('score_versions', true);
REVOKE UPDATE ON score_versions FROM thelife_app;
GRANT UPDATE (is_current) ON score_versions TO thelife_app;

CREATE VIEW scores WITH (security_invoker = true, security_barrier = true) AS
SELECT id, tenant_id, created_at, updated_at, version, created_by_kind, created_by_id, origin, source_message_id,
       deleted_at, user_id, period, period_start, profile, formula_version, breakdown, consistency, coverage, base,
       adjustment_pct, adjustment, final, status
  FROM score_versions WHERE is_current;
REVOKE INSERT, UPDATE, DELETE ON scores FROM thelife_app;
GRANT SELECT ON scores TO thelife_app;

CREATE TABLE preemptions (
  id              uuid PRIMARY KEY,
  tenant_id       uuid NOT NULL REFERENCES tenants(id),
  user_id         uuid NOT NULL REFERENCES users(id),
  local_day       date NOT NULL,
  kind            text NOT NULL CHECK (kind IN ('overlap', 'while_due')),
  displaced_type  text NOT NULL,
  displaced_id    uuid NOT NULL,
  displaced_title text,
  minutes         integer NOT NULL CHECK (minutes > 0),
  created_at      timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX preemptions_day ON preemptions (tenant_id, user_id, local_day);
ALTER TABLE preemptions ENABLE ROW LEVEL SECURITY;
ALTER TABLE preemptions FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON preemptions USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());
GRANT DELETE ON preemptions TO thelife_app;   -- replaced when a day is recomputed

CREATE TABLE score_disputes (
  id          uuid PRIMARY KEY,
  tenant_id   uuid NOT NULL REFERENCES tenants(id),
  user_id     uuid NOT NULL REFERENCES users(id),
  score_id    uuid NOT NULL REFERENCES score_versions(id),
  reason      text NOT NULL CHECK (length(reason) BETWEEN 1 AND 1000),
  status      text NOT NULL DEFAULT 'queued' CHECK (status IN ('queued', 'done')),
  before_pct  numeric(4,1),
  after_pct   numeric(4,1),
  created_at  timestamptz NOT NULL DEFAULT now(),
  resolved_at timestamptz
);
ALTER TABLE score_disputes ENABLE ROW LEVEL SECURITY;
ALTER TABLE score_disputes FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON score_disputes USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());

-- migrate:down
DROP TABLE score_disputes;
DROP TABLE preemptions;
DROP VIEW scores;
DROP TABLE score_versions;
DROP TABLE scoring_settings;
DROP TABLE work_contexts;
DROP TABLE targets;
DROP TABLE day_profile_overrides;
DROP TABLE day_profiles;
