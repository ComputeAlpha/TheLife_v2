-- migrate:up
-- Detections (append-only, backend_lld.md §6.5) and activities, which may be confirmed from a detection.
--
-- Invariants enforced here, not in the backend:
--   * thelife_app may INSERT detections but never UPDATE or DELETE them, and never writes detection_events.
--   * Content columns never change. Only status and superseded_by_id move, and only through
--     app.detection_transition() / app.detection_claim() (SECURITY DEFINER, owned by the table owner, which is
--     still subject to the forced RLS policy).
--   * Every status change writes a detection_events row (AFTER UPDATE trigger), so history cannot be skipped.
--   * New rows start as 'detected' (or 'confirmed' when the user stated them); supersedes_id is set only by a claim.

CREATE TABLE detections (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz CHECK (deleted_at IS NULL),  -- never deleted
  source           text NOT NULL CHECK (source IN ('vision', 'process', 'user', 'external')),
  device_id        uuid,
  observation_ids  uuid[] NOT NULL DEFAULT '{}',
  started_at       timestamptz NOT NULL,
  ended_at         timestamptz NOT NULL,
  area             area NOT NULL,
  category         text NOT NULL CHECK (length(category) BETWEEN 1 AND 80),
  summary          text NOT NULL CHECK (length(summary) BETWEEN 1 AND 500),
  confidence       real NOT NULL DEFAULT 1 CHECK (confidence BETWEEN 0 AND 1),
  evidence         jsonb CHECK (evidence IS NULL OR jsonb_typeof(evidence) = 'object'),
  status           text NOT NULL DEFAULT 'detected'
                   CHECK (status IN ('detected', 'confirmed', 'disputed', 'superseded')),
  supersedes_id    uuid,
  superseded_by_id uuid,
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, device_id) REFERENCES devices (tenant_id, id),
  FOREIGN KEY (tenant_id, supersedes_id) REFERENCES detections (tenant_id, id),
  FOREIGN KEY (tenant_id, superseded_by_id) REFERENCES detections (tenant_id, id),
  CHECK (ended_at >= started_at),
  CHECK ((status = 'superseded') = (superseded_by_id IS NOT NULL))
);
SELECT app.setup_tenant_table('detections', true);
CREATE INDEX detections_time_idx ON detections (tenant_id, started_at);
REVOKE UPDATE ON detections FROM thelife_app;

CREATE TABLE detection_events (
  id           uuid PRIMARY KEY,
  tenant_id    uuid NOT NULL REFERENCES tenants(id),
  detection_id uuid NOT NULL,
  at           timestamptz NOT NULL DEFAULT now(),
  actor_kind   principal_kind NOT NULL,
  actor_id     uuid,
  from_status  text NOT NULL,
  to_status    text NOT NULL,
  reason       text CHECK (length(reason) <= 1000),
  FOREIGN KEY (tenant_id, detection_id) REFERENCES detections (tenant_id, id)
);
ALTER TABLE detection_events ENABLE ROW LEVEL SECURITY;
ALTER TABLE detection_events FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON detection_events
  USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());
CREATE INDEX detection_events_detection_idx ON detection_events (tenant_id, detection_id, at);
REVOKE INSERT, UPDATE ON detection_events FROM thelife_app;

-- The acting principal from the TenantTx settings (api_key callers are recorded as external).
CREATE FUNCTION app.current_actor_kind() RETURNS principal_kind LANGUAGE sql STABLE AS $$
  SELECT CASE coalesce(nullif(current_setting('app.principal_kind', true), ''), 'system')
           WHEN 'api_key' THEN 'external'
           ELSE coalesce(nullif(current_setting('app.principal_kind', true), ''), 'system') END::public.principal_kind
$$;
CREATE FUNCTION app.current_actor_id() RETURNS uuid LANGUAGE sql STABLE AS $$
  SELECT nullif(current_setting('app.principal_id', true), '')::uuid
$$;

CREATE FUNCTION app.detection_before_insert() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.supersedes_id IS NOT NULL AND current_user <> (SELECT pg_catalog.pg_get_userbyid(c.relowner)
                                                          FROM pg_catalog.pg_class c WHERE c.oid = TG_RELID) THEN
    RAISE EXCEPTION 'supersedes_id is set only by a claim' USING ERRCODE = 'TL422';
  END IF;
  NEW.status := CASE WHEN NEW.source = 'user' THEN 'confirmed' ELSE 'detected' END;
  NEW.superseded_by_id := NULL;
  RETURN NEW;
END $$;
CREATE TRIGGER before_insert BEFORE INSERT ON detections
  FOR EACH ROW EXECUTE FUNCTION app.detection_before_insert();

CREATE FUNCTION app.detection_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF (pg_catalog.to_jsonb(NEW) - '{status,superseded_by_id,updated_at,version}'::text[])
     IS DISTINCT FROM (pg_catalog.to_jsonb(OLD) - '{status,superseded_by_id,updated_at,version}'::text[]) THEN
    RAISE EXCEPTION 'detections are append-only' USING ERRCODE = 'TL409';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER append_only BEFORE UPDATE ON detections
  FOR EACH ROW EXECUTE FUNCTION app.detection_append_only();

CREATE FUNCTION app.detection_log_event() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO public.detection_events (id, tenant_id, detection_id, at, actor_kind, actor_id, from_status,
                                       to_status, reason)
  VALUES (app.uuid_v7(), NEW.tenant_id, NEW.id, pg_catalog.now(), app.current_actor_kind(), app.current_actor_id(),
          OLD.status, NEW.status, nullif(pg_catalog.current_setting('app.transition_reason', true), ''));
  RETURN NULL;
END $$;
CREATE TRIGGER log_event AFTER UPDATE OF status ON detections
  FOR EACH ROW WHEN (OLD.status IS DISTINCT FROM NEW.status) EXECUTE FUNCTION app.detection_log_event();

-- confirm: detected|disputed → confirmed; dispute: detected|confirmed → disputed.
CREATE FUNCTION app.detection_transition(p_id uuid, p_action text, p_reason text)
  RETURNS public.detections LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $$
DECLARE d public.detections; target text;
BEGIN
  target := CASE p_action WHEN 'confirm' THEN 'confirmed' WHEN 'dispute' THEN 'disputed' END;
  IF target IS NULL THEN RAISE EXCEPTION 'unknown action' USING ERRCODE = 'TL422'; END IF;
  SELECT * INTO d FROM public.detections WHERE id = p_id FOR UPDATE;
  IF NOT FOUND THEN RAISE EXCEPTION 'Not found.' USING ERRCODE = 'TL404'; END IF;
  IF d.status = 'superseded' THEN
    RAISE EXCEPTION 'This detection was replaced by a claim; act on the newer one.' USING ERRCODE = 'TL409';
  END IF;
  IF d.status = target THEN
    RAISE EXCEPTION 'The detection is already %.', target USING ERRCODE = 'TL409';
  END IF;
  PERFORM set_config('app.transition_reason', coalesce(p_reason, ''), true);
  UPDATE public.detections SET status = target WHERE id = p_id RETURNING * INTO d;
  PERFORM set_config('app.transition_reason', '', true);
  RETURN d;
END $$;

-- claim: a new user detection replaces the old one, which becomes superseded.
CREATE FUNCTION app.detection_claim(p_id uuid, p_area text, p_category text, p_summary text,
                                    p_started timestamptz, p_ended timestamptz)
  RETURNS public.detections LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $$
DECLARE old public.detections; created public.detections; v_id uuid := app.uuid_v7();
BEGIN
  SELECT * INTO old FROM public.detections WHERE id = p_id FOR UPDATE;
  IF NOT FOUND THEN RAISE EXCEPTION 'Not found.' USING ERRCODE = 'TL404'; END IF;
  IF old.status = 'superseded' THEN
    RAISE EXCEPTION 'This detection was already replaced by a claim.' USING ERRCODE = 'TL409';
  END IF;
  INSERT INTO public.detections (id, tenant_id, created_by_kind, created_by_id, source, started_at, ended_at,
                                 area, category, summary, confidence, supersedes_id)
  VALUES (v_id, old.tenant_id, app.current_actor_kind(), app.current_actor_id(), 'user',
          coalesce(p_started, old.started_at), coalesce(p_ended, old.ended_at),
          p_area::public.area, p_category, p_summary, 1, old.id)
  RETURNING * INTO created;
  UPDATE public.detections SET status = 'superseded', superseded_by_id = v_id WHERE id = old.id;
  RETURN created;
END $$;

-- ---------------------------------------------------------------- activities
CREATE TABLE activities (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  area         area NOT NULL,
  category     text CHECK (length(category) <= 80),
  title        text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  started_at   timestamptz NOT NULL,
  ended_at     timestamptz NOT NULL,
  source       text NOT NULL DEFAULT 'manual' CHECK (source IN ('manual', 'detection', 'agent')),
  detection_id uuid,
  link_type    text CHECK (link_type IN ('task', 'goal', 'skill_resource', 'project', 'deliverable',
                                         'entertainment_item')),
  link_id      uuid,
  notes        text CHECK (length(notes) <= 10000),
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, detection_id) REFERENCES detections (tenant_id, id),
  CHECK (ended_at >= started_at),
  CHECK (source <> 'detection' OR detection_id IS NOT NULL),
  CHECK ((link_type IS NULL) = (link_id IS NULL))
);
SELECT app.setup_tenant_table('activities', true);
CREATE INDEX activities_time_idx ON activities (tenant_id, started_at) WHERE deleted_at IS NULL;
CREATE TRIGGER check_link BEFORE INSERT OR UPDATE OF link_type, link_id ON activities
  FOR EACH ROW EXECUTE FUNCTION app.check_link();

-- migrate:down
DROP TABLE activities;
DROP FUNCTION app.detection_claim(uuid, text, text, text, timestamptz, timestamptz);
DROP FUNCTION app.detection_transition(uuid, text, text);
DROP TABLE detection_events;
DROP TABLE detections;
DROP FUNCTION app.detection_log_event();
DROP FUNCTION app.detection_append_only();
DROP FUNCTION app.detection_before_insert();
DROP FUNCTION app.current_actor_id();
DROP FUNCTION app.current_actor_kind();
