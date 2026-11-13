-- migrate:up
-- Background jobs (backend_lld.md §10) and memory tiers (agent_lld.md §6): episodic summaries and the long-term
-- registry (entities and facts).

-- ---------------------------------------------------------------- jobs
-- Deployment-level queue (ops schema, like provider_state): the worker claims across tenants and opens a TenantTx
-- for the job's tenant before touching tenant data. Payloads hold ids and dates only, never content.
CREATE TABLE ops.jobs (
  id           uuid PRIMARY KEY,
  tenant_id    uuid REFERENCES tenants(id),   -- NULL for deployment-wide jobs
  kind         text NOT NULL CHECK (kind ~ '^[a-z0-9_.]{1,64}$'),
  payload      jsonb NOT NULL DEFAULT '{}'::jsonb,
  dedupe_key   text,
  priority     smallint NOT NULL DEFAULT 3,   -- 0 interactive … 3 background (catch-up order, agent_lld.md §3.5)
  run_at       timestamptz NOT NULL DEFAULT now(),
  status       text NOT NULL DEFAULT 'queued' CHECK (status IN ('queued', 'running', 'done', 'dead')),
  attempts     integer NOT NULL DEFAULT 0,
  max_attempts integer NOT NULL DEFAULT 5,
  locked_by    text,
  locked_at    timestamptz,
  last_error   text CHECK (length(last_error) <= 1000),
  created_at   timestamptz NOT NULL DEFAULT now(),
  updated_at   timestamptz NOT NULL DEFAULT now(),
  finished_at  timestamptz
);
CREATE INDEX jobs_ready ON ops.jobs (priority, run_at) WHERE status = 'queued';
-- At most one queued or running job per dedupe key: a burst of triggers coalesces into one job.
CREATE UNIQUE INDEX jobs_dedupe ON ops.jobs (kind, dedupe_key) WHERE dedupe_key IS NOT NULL AND status IN ('queued', 'running');
GRANT SELECT, INSERT, UPDATE ON ops.jobs TO thelife_app;

-- Each user's local day is closed exactly once (agent_lld.md §6.2, day close at day_start_hour).
CREATE TABLE ops.day_closes (
  user_id    uuid NOT NULL REFERENCES users(id),
  local_day  date NOT NULL,
  tenant_id  uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (user_id, local_day)
);

GRANT SELECT ON ops.day_closes TO thelife_app;  -- ids and dates only

-- Cross-tenant scheduling (one of the few cross-tenant paths, backend_lld.md §4.3): queues a day.close job for
-- every active user whose previous local day has not been closed yet. Returns the number queued.
CREATE FUNCTION ops.enqueue_day_closes() RETURNS integer
  LANGUAGE sql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
  WITH due AS (
    SELECT u.tenant_id, u.id AS user_id,
           ((now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date - 1 AS local_day
      FROM public.users u JOIN public.tenants t ON t.id = u.tenant_id
     WHERE u.status = 'active' AND t.status = 'active'),
  fresh AS (
    INSERT INTO ops.day_closes (user_id, local_day, tenant_id)
    SELECT user_id, local_day, tenant_id FROM due ON CONFLICT DO NOTHING
    RETURNING user_id, local_day, tenant_id),
  queued AS (
    INSERT INTO ops.jobs (id, tenant_id, kind, payload, dedupe_key, priority)
    SELECT gen_random_uuid(), tenant_id, 'day.close',
           jsonb_build_object('user_id', user_id, 'local_day', local_day),
           user_id::text || ':' || local_day::text, 2
      FROM fresh
    RETURNING 1)
  SELECT count(*)::integer FROM queued
$$;

-- Turns left running by a crash (no worker will finish them) are marked failed after 10 minutes.
CREATE FUNCTION ops.fail_stale_turns() RETURNS integer
  LANGUAGE sql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
  WITH t AS (
    UPDATE public.turns SET status = 'failed', error = 'interrupted', finished_at = now(), updated_at = now()
     WHERE status IN ('queued', 'running') AND updated_at < now() - interval '10 minutes'
    RETURNING 1)
  SELECT count(*)::integer FROM t
$$;

-- Stored idempotent responses are kept 24 h (backend_lld.md §7).
CREATE FUNCTION ops.gc_idempotency_keys() RETURNS integer
  LANGUAGE sql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
  WITH d AS (DELETE FROM public.idempotency_keys WHERE created_at < now() - interval '24 hours' RETURNING 1)
  SELECT count(*)::integer FROM d
$$;

GRANT USAGE, CREATE ON SCHEMA ops TO thelife_definer;
GRANT SELECT ON users, tenants TO thelife_definer;
GRANT SELECT, INSERT ON ops.day_closes TO thelife_definer;
GRANT SELECT, INSERT ON ops.jobs TO thelife_definer;
GRANT SELECT, UPDATE ON turns TO thelife_definer;
GRANT SELECT, DELETE ON idempotency_keys TO thelife_definer;
ALTER FUNCTION ops.enqueue_day_closes() OWNER TO thelife_definer;
ALTER FUNCTION ops.fail_stale_turns() OWNER TO thelife_definer;
ALTER FUNCTION ops.gc_idempotency_keys() OWNER TO thelife_definer;
REVOKE EXECUTE ON FUNCTION ops.enqueue_day_closes(), ops.fail_stale_turns(), ops.gc_idempotency_keys() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION ops.enqueue_day_closes(), ops.fail_stale_turns(), ops.gc_idempotency_keys() TO thelife_app;

-- ---------------------------------------------------------------- episodic memory
CREATE TABLE episodic_summaries (
  id             uuid PRIMARY KEY,
  tenant_id      uuid NOT NULL REFERENCES tenants(id),
  user_id        uuid NOT NULL REFERENCES users(id),
  channel        text NOT NULL CHECK (channel IN ('web', 'telegram')),
  period         text NOT NULL CHECK (period IN ('day', 'week', 'month')),
  period_start   date NOT NULL,
  content        text NOT NULL CHECK (length(content) BETWEEN 1 AND 8000),
  topics         text[] NOT NULL DEFAULT '{}',
  model          text NOT NULL,
  prompt_version text NOT NULL,
  created_at     timestamptz NOT NULL DEFAULT now(),
  UNIQUE (tenant_id, user_id, channel, period, period_start)
);
ALTER TABLE episodic_summaries ENABLE ROW LEVEL SECURITY;
ALTER TABLE episodic_summaries FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON episodic_summaries USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());

-- ---------------------------------------------------------------- long-term registry (§6.4)
CREATE TABLE kg_entities (
  id                uuid PRIMARY KEY,
  tenant_id         uuid NOT NULL REFERENCES tenants(id),
  created_at        timestamptz NOT NULL DEFAULT now(),
  updated_at        timestamptz NOT NULL DEFAULT now(),
  version           integer NOT NULL DEFAULT 1,
  created_by_kind   principal_kind NOT NULL,
  created_by_id     uuid,
  origin            origin_kind NOT NULL DEFAULT 'direct',
  source_message_id uuid,
  deleted_at        timestamptz,
  user_id           uuid NOT NULL REFERENCES users(id),
  kind              text NOT NULL CHECK (kind IN ('person', 'project', 'goal', 'habit', 'preference', 'place', 'app',
                                                  'topic', 'self', 'other')),
  name              text NOT NULL CHECK (length(name) BETWEEN 1 AND 120),
  aliases           text[] NOT NULL DEFAULT '{}',
  summary           text CHECK (length(summary) <= 280),
  salience          real NOT NULL DEFAULT 1.0 CHECK (salience >= 0),
  pinned            boolean NOT NULL DEFAULT false,
  last_used_at      timestamptz
);
CREATE UNIQUE INDEX kg_entities_name ON kg_entities (tenant_id, user_id, lower(name)) WHERE deleted_at IS NULL;
SELECT app.setup_tenant_table('kg_entities', true);

-- Facts are append-only like detections: a change is a new fact; the old one gets superseded_by.
CREATE TABLE kg_facts (
  id                uuid PRIMARY KEY,
  tenant_id         uuid NOT NULL REFERENCES tenants(id),
  created_at        timestamptz NOT NULL DEFAULT now(),
  updated_at        timestamptz NOT NULL DEFAULT now(),
  version           integer NOT NULL DEFAULT 1,
  created_by_kind   principal_kind NOT NULL,
  created_by_id     uuid,
  origin            origin_kind NOT NULL DEFAULT 'direct',
  source_message_id uuid,
  deleted_at        timestamptz,              -- forgotten
  user_id           uuid NOT NULL REFERENCES users(id),
  subject_id        uuid NOT NULL REFERENCES kg_entities(id),
  predicate         text CHECK (length(predicate) <= 80),
  object_id         uuid REFERENCES kg_entities(id),
  value             text CHECK (length(value) <= 280),
  statement         text NOT NULL CHECK (length(statement) BETWEEN 1 AND 280),
  confidence        real NOT NULL DEFAULT 1.0 CHECK (confidence BETWEEN 0 AND 1),
  status            text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'proposed', 'superseded')),
  valid_from        timestamptz NOT NULL DEFAULT now(),
  valid_to          timestamptz,
  superseded_by     uuid REFERENCES kg_facts(id),
  source_refs       jsonb NOT NULL DEFAULT '[]'::jsonb,
  last_used_at      timestamptz,
  CHECK ((status = 'superseded') = (superseded_by IS NOT NULL))
);
CREATE INDEX kg_facts_subject ON kg_facts (subject_id) WHERE deleted_at IS NULL AND status = 'active';
CREATE INDEX kg_facts_text ON kg_facts USING gin (to_tsvector('simple', statement));
SELECT app.setup_tenant_table('kg_facts', true);
-- The version (ETag) changes with the fact's state only: recalling a fact (last_used_at) must not make a pending
-- approval that pinned it stale.
DROP TRIGGER touch ON kg_facts;
CREATE TRIGGER touch BEFORE UPDATE ON kg_facts FOR EACH ROW
  WHEN (OLD.status IS DISTINCT FROM NEW.status OR OLD.deleted_at IS DISTINCT FROM NEW.deleted_at
        OR OLD.superseded_by IS DISTINCT FROM NEW.superseded_by OR OLD.valid_to IS DISTINCT FROM NEW.valid_to)
  EXECUTE FUNCTION app.touch();
-- Content is immutable; only status, supersession, validity, use and soft delete change.
REVOKE UPDATE ON kg_facts FROM thelife_app;
GRANT UPDATE (status, superseded_by, valid_to, last_used_at, deleted_at) ON kg_facts TO thelife_app;

-- Keyword search over the transcript until semantic search exists (history.search).
CREATE INDEX messages_text ON messages USING gin (to_tsvector('simple', content)) WHERE role IN ('user', 'assistant');

-- migrate:down
DROP INDEX messages_text;
DROP TABLE kg_facts;
DROP TABLE kg_entities;
DROP TABLE episodic_summaries;
DROP FUNCTION ops.gc_idempotency_keys();
DROP FUNCTION ops.fail_stale_turns();
DROP FUNCTION ops.enqueue_day_closes();
REVOKE SELECT, UPDATE ON turns FROM thelife_definer;
REVOKE SELECT, DELETE ON idempotency_keys FROM thelife_definer;
DROP TABLE ops.day_closes;
DROP TABLE ops.jobs;
