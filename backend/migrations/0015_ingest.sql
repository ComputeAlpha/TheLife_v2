-- migrate:up
-- Collector ingest (backend_lld.md §6.6, §7.2, §7.4; collector_lld.md): raw observations, device liveness,
-- per-user classification rules and the derivation cursor.

-- Raw facts from the collector. The payload (window titles, process names) is sealed like the other encrypted
-- fields (migration 0014): the backend opens it to derive detections. Kept 30 days (ops.observations_retention).
CREATE TABLE observations (
  id          uuid PRIMARY KEY,
  tenant_id   uuid NOT NULL REFERENCES tenants(id),
  device_id   uuid NOT NULL,
  client_seq  bigint NOT NULL CHECK (client_seq >= 0),
  captured_at timestamptz NOT NULL,
  received_at timestamptz NOT NULL DEFAULT now(),
  kind        text NOT NULL CHECK (kind IN ('window', 'idle', 'process_snapshot', 'capture_meta', 'gap', 'session')),
  payload     bytea NOT NULL CHECK (octet_length(payload) <= 262144),
  UNIQUE (tenant_id, device_id, client_seq),
  FOREIGN KEY (tenant_id, device_id) REFERENCES devices (tenant_id, id)
);
ALTER TABLE observations ENABLE ROW LEVEL SECURITY;
ALTER TABLE observations FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON observations
  USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());
CREATE INDEX observations_device_time ON observations (tenant_id, device_id, captured_at);
CREATE INDEX observations_age ON observations (captured_at);
-- Facts are appended, never edited; deletion is retention only (a definer function).
REVOKE UPDATE, DELETE ON observations FROM thelife_app;

-- How far each device's observations have been turned into detections.
CREATE TABLE ingest_state (
  tenant_id     uuid NOT NULL REFERENCES tenants(id),
  device_id     uuid PRIMARY KEY,
  derived_until timestamptz NOT NULL,
  updated_at    timestamptz NOT NULL DEFAULT now(),
  FOREIGN KEY (tenant_id, device_id) REFERENCES devices (tenant_id, id)
);
ALTER TABLE ingest_state ENABLE ROW LEVEL SECURITY;
ALTER TABLE ingest_state FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON ingest_state
  USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());

-- The last heartbeat (queue depth, pause, X state, last capture, last error). last_seen_at already exists.
ALTER TABLE devices ADD COLUMN status jsonb NOT NULL DEFAULT '{}'::jsonb
  CHECK (jsonb_typeof(status) = 'object' AND octet_length(status::text) <= 2048);

-- Token rotation: the old device token stays valid for 5 minutes (expires_at).
GRANT UPDATE (expires_at) ON credentials TO thelife_app;

-- The user's own classification rules, checked before the built-in defaults (config/ingest_rules.json).
CREATE TABLE ingest_rules (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  match_wm_class text CHECK (length(match_wm_class) <= 256),
  match_exe      text CHECK (length(match_exe) <= 4096),
  match_title    text CHECK (length(match_title) BETWEEN 2 AND 200),
  app            text NOT NULL CHECK (length(app) BETWEEN 1 AND 80),
  category       text NOT NULL CHECK (length(category) BETWEEN 1 AND 80),
  area           area NOT NULL,
  confidence     real NOT NULL DEFAULT 0.7 CHECK (confidence BETWEEN 0.1 AND 1),
  enabled        boolean NOT NULL DEFAULT true,
  CHECK (coalesce(match_wm_class, match_exe, match_title) IS NOT NULL)
);
SELECT app.setup_tenant_table('ingest_rules', true);

-- Retention (30 days), across tenants like the other housekeeping functions. Returns the number removed.
CREATE FUNCTION ops.observations_retention() RETURNS integer
  LANGUAGE sql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
  WITH d AS (
    DELETE FROM public.observations WHERE id IN (
      SELECT id FROM public.observations WHERE captured_at < now() - interval '30 days' LIMIT 20000)
    RETURNING 1)
  SELECT count(*)::integer FROM d
$$;
GRANT SELECT, DELETE ON observations TO thelife_definer;
ALTER FUNCTION ops.observations_retention() OWNER TO thelife_definer;
REVOKE EXECUTE ON FUNCTION ops.observations_retention() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION ops.observations_retention() TO thelife_app;

-- migrate:down
DROP FUNCTION ops.observations_retention();
DROP TABLE ingest_rules;
REVOKE UPDATE (expires_at) ON credentials FROM thelife_app;
ALTER TABLE devices DROP COLUMN status;
DROP TABLE ingest_state;
DROP TABLE observations;
