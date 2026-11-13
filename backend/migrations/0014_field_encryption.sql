-- migrate:up
-- Envelope encryption of selected free-text fields (backend_lld.md §9, decided 2026-11-13): each tenant has a
-- data key (DEK), wrapped by the deployment key (KEK, from THELIFE_KEK_FILE or THELIFE_KEK) and stored here. The
-- encrypted columns become bytea, opaque to SQL: the app seals and opens them (backend/src/db/field_crypt.*).
--
-- Value format: 0x01 | 24-byte nonce | XChaCha20-Poly1305 ciphertext of the JSON text of the value, with
-- "<tenant>|<table>.<column>" as associated data. 0x00 | JSON text marks a value written before this migration;
-- the fieldcrypt.reseal job queued below seals those.

CREATE TABLE tenant_keys (
  tenant_id  uuid PRIMARY KEY REFERENCES tenants(id),
  kek_id     text NOT NULL CHECK (kek_id ~ '^[0-9a-f]{16}$'),  -- which KEK wrapped it (sha256 prefix, not secret)
  wrapped    bytea NOT NULL CHECK (octet_length(wrapped) = 1 + 24 + 32 + 16),
  created_at timestamptz NOT NULL DEFAULT now()
);
ALTER TABLE tenant_keys ENABLE ROW LEVEL SECURITY;
ALTER TABLE tenant_keys FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON tenant_keys
  USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());
-- Created once, never changed or removed by the app (rotation is an operator task).
REVOKE ALL ON tenant_keys FROM thelife_app;
GRANT SELECT, INSERT ON tenant_keys TO thelife_app;

-- Converts a column to the legacy-marked bytea form and replaces its length check by a byte cap.
CREATE FUNCTION app.encrypt_column(tbl regclass, col name, is_json boolean) RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record;
BEGIN
  FOR c IN SELECT con.conname FROM pg_catalog.pg_constraint con
            JOIN pg_catalog.pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = ANY (con.conkey)
           WHERE con.conrelid = tbl AND con.contype = 'c' AND a.attname = col
             AND pg_catalog.array_length(con.conkey, 1) = 1 LOOP
    EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %I', tbl, c.conname);
  END LOOP;
  EXECUTE format('ALTER TABLE %s ALTER COLUMN %I TYPE bytea USING CASE WHEN %I IS NULL THEN NULL '
                 'ELSE ''\x00''::bytea || convert_to(%s, ''UTF8'') END',
                 tbl, col, col, CASE WHEN is_json THEN format('%I::text', col) ELSE format('to_jsonb(%I)::text', col) END);
  EXECUTE format('ALTER TABLE %s ADD CONSTRAINT %I CHECK (octet_length(%I) <= 65536)', tbl, col || '_sealed', col);
END $$;

SELECT app.encrypt_column('goals', 'description', false);
SELECT app.encrypt_column('plans', 'body', false);
SELECT app.encrypt_column('work_roles', 'description', false);
SELECT app.encrypt_column('work_responsibilities', 'description', false);
SELECT app.encrypt_column('work_projects', 'description', false);
SELECT app.encrypt_column('work_deliverables', 'description', false);
SELECT app.encrypt_column('skill_sessions', 'notes', false);
SELECT app.encrypt_column('entertainment_slots', 'notes', false);
SELECT app.encrypt_column('tasks', 'notes', false);
SELECT app.encrypt_column('calendar_events', 'notes', false);
SELECT app.encrypt_column('activities', 'notes', false);
SELECT app.encrypt_column('detections', 'summary', false);
SELECT app.encrypt_column('detections', 'evidence', true);
ALTER TABLE detections ALTER COLUMN summary SET NOT NULL;

-- Detections are append-only; the reseal job may replace a legacy (0x00) value of summary/evidence by a sealed
-- one (0x01) through this function and nothing else.
CREATE OR REPLACE FUNCTION app.detection_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE skip text[] := '{status,superseded_by_id,updated_at,version}';
BEGIN
  IF pg_catalog.current_setting('app.reseal', true) = 'on' THEN
    IF pg_catalog.get_byte(OLD.summary, 0) = 0 AND pg_catalog.get_byte(NEW.summary, 0) = 1 THEN
      skip := skip || '{summary}'::text[];
    END IF;
    IF OLD.evidence IS NOT NULL AND pg_catalog.get_byte(OLD.evidence, 0) = 0
       AND NEW.evidence IS NOT NULL AND pg_catalog.get_byte(NEW.evidence, 0) = 1 THEN
      skip := skip || '{evidence}'::text[];
    END IF;
  END IF;
  IF (pg_catalog.to_jsonb(NEW) - skip) IS DISTINCT FROM (pg_catalog.to_jsonb(OLD) - skip) THEN
    RAISE EXCEPTION 'detections are append-only' USING ERRCODE = 'TL409';
  END IF;
  RETURN NEW;
END $$;

CREATE FUNCTION app.detection_reseal(p_id uuid, p_summary bytea, p_evidence bytea) RETURNS void
  LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $$
BEGIN
  PERFORM set_config('app.reseal', 'on', true);
  UPDATE public.detections SET summary = coalesce(p_summary, summary), evidence = coalesce(p_evidence, evidence)
   WHERE id = p_id;
  PERFORM set_config('app.reseal', '', true);
END $$;

-- The claim now receives the summary sealed by the app.
DROP FUNCTION app.detection_claim(uuid, text, text, text, timestamptz, timestamptz);
CREATE FUNCTION app.detection_claim(p_id uuid, p_area text, p_category text, p_summary bytea,
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

-- Owned by the table owner like the 0005 functions (detection_before_insert lets only the owner set supersedes_id).
DO $$
DECLARE f text;
BEGIN
  FOREACH f IN ARRAY ARRAY['app.detection_reseal(uuid, bytea, bytea)',
                           'app.detection_claim(uuid, text, text, bytea, timestamptz, timestamptz)'] LOOP
    EXECUTE format('REVOKE EXECUTE ON FUNCTION %s FROM PUBLIC', f);
    EXECUTE format('GRANT EXECUTE ON FUNCTION %s TO thelife_app', f);
  END LOOP;
END $$;

-- Seal what was written before this migration: one job per tenant. tenants is under forced RLS, so the listing
-- runs as thelife_definer (like ops.enqueue_day_closes), in a function that exists only during this migration.
CREATE FUNCTION ops.enqueue_reseal() RETURNS integer
  LANGUAGE sql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
  WITH q AS (
    INSERT INTO ops.jobs (id, tenant_id, kind, dedupe_key)
    SELECT gen_random_uuid(), t.id, 'fieldcrypt.reseal', 'reseal:' || t.id::text FROM public.tenants t
    RETURNING 1)
  SELECT count(*)::integer FROM q
$$;
ALTER FUNCTION ops.enqueue_reseal() OWNER TO thelife_definer;
SELECT ops.enqueue_reseal();
DROP FUNCTION ops.enqueue_reseal();

-- migrate:down
DELETE FROM ops.jobs WHERE kind = 'fieldcrypt.reseal';
DROP FUNCTION app.detection_claim(uuid, text, text, bytea, timestamptz, timestamptz);
DROP FUNCTION app.detection_reseal(uuid, bytea, bytea);
-- Down works only while every value is still in the legacy form (0x00 | JSON text); sealed values cannot be
-- decrypted by SQL, so the conversion refuses them.
CREATE FUNCTION app.refuse_sealed(what text) RETURNS text LANGUAGE plpgsql VOLATILE AS $$
BEGIN
  RAISE EXCEPTION 'a sealed value in % cannot be migrated down', what;
END $$;
CREATE FUNCTION app.decrypt_column_legacy(tbl regclass, col name, is_json boolean) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %I', tbl, col || '_sealed');
  EXECUTE format('ALTER TABLE %s ALTER COLUMN %I TYPE %s USING CASE WHEN %I IS NULL THEN NULL '
                 'WHEN get_byte(%I, 0) <> 0 THEN app.refuse_sealed(%L)::%s '
                 'ELSE %s END',
                 tbl, col, CASE WHEN is_json THEN 'jsonb' ELSE 'text' END, col, col, tbl || '.' || col,
                 CASE WHEN is_json THEN 'jsonb' ELSE 'text' END,
                 CASE WHEN is_json THEN format('convert_from(substring(%I from 2), ''UTF8'')::jsonb', col)
                      ELSE format('convert_from(substring(%I from 2), ''UTF8'')::jsonb #>> ''{}''', col) END);
END $$;
SELECT app.decrypt_column_legacy('goals', 'description', false);
SELECT app.decrypt_column_legacy('plans', 'body', false);
SELECT app.decrypt_column_legacy('work_roles', 'description', false);
SELECT app.decrypt_column_legacy('work_responsibilities', 'description', false);
SELECT app.decrypt_column_legacy('work_projects', 'description', false);
SELECT app.decrypt_column_legacy('work_deliverables', 'description', false);
SELECT app.decrypt_column_legacy('skill_sessions', 'notes', false);
SELECT app.decrypt_column_legacy('entertainment_slots', 'notes', false);
SELECT app.decrypt_column_legacy('tasks', 'notes', false);
SELECT app.decrypt_column_legacy('calendar_events', 'notes', false);
SELECT app.decrypt_column_legacy('activities', 'notes', false);
SELECT app.decrypt_column_legacy('detections', 'summary', false);
SELECT app.decrypt_column_legacy('detections', 'evidence', true);
DROP FUNCTION app.decrypt_column_legacy(regclass, name, boolean);
DROP FUNCTION app.refuse_sealed(text);
DROP FUNCTION app.encrypt_column(regclass, name, boolean);
CREATE OR REPLACE FUNCTION app.detection_append_only() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF (pg_catalog.to_jsonb(NEW) - '{status,superseded_by_id,updated_at,version}'::text[])
     IS DISTINCT FROM (pg_catalog.to_jsonb(OLD) - '{status,superseded_by_id,updated_at,version}'::text[]) THEN
    RAISE EXCEPTION 'detections are append-only' USING ERRCODE = 'TL409';
  END IF;
  RETURN NEW;
END $$;
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
DROP TABLE tenant_keys;
