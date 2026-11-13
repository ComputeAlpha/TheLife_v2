-- migrate:up
-- Identity and access (backend_lld.md §4, §5, §6.1): shared types and triggers, tenants, users, devices,
-- credentials, and the only cross-tenant paths: auth.* (login/token lookup) and admin.* (bootstrap CLI).

CREATE TYPE principal_kind  AS ENUM ('user', 'agent', 'external', 'device', 'system');
CREATE TYPE origin_kind     AS ENUM ('direct', 'user_stated', 'inferred', 'imported');
CREATE TYPE credential_kind AS ENUM ('user_session', 'device', 'api_key');

-- ---------------------------------------------------------------- shared helpers
-- Bumps updated_at and version on every UPDATE (version is the ETag; clients send it back in If-Match).
CREATE FUNCTION app.touch() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  NEW.updated_at := now();
  NEW.version := OLD.version + 1;
  RETURN NEW;
END $$;

-- Identity and provenance columns of a resource never change after insert.
CREATE FUNCTION app.guard_resource_immutables() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.id IS DISTINCT FROM OLD.id OR NEW.tenant_id IS DISTINCT FROM OLD.tenant_id
     OR NEW.created_at IS DISTINCT FROM OLD.created_at
     OR NEW.created_by_kind IS DISTINCT FROM OLD.created_by_kind
     OR NEW.created_by_id IS DISTINCT FROM OLD.created_by_id
     OR NEW.origin IS DISTINCT FROM OLD.origin
     OR NEW.source_message_id IS DISTINCT FROM OLD.source_message_id THEN
    RAISE EXCEPTION 'immutable column changed on %', TG_TABLE_NAME USING ERRCODE = 'check_violation';
  END IF;
  RETURN NEW;
END $$;

-- Enables and forces RLS with the standard tenant policy, and installs the touch trigger.
-- resource=true also installs the immutables guard (tables with the common resource columns).
CREATE FUNCTION app.setup_tenant_table(tbl regclass, resource boolean) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE format('ALTER TABLE %s ENABLE ROW LEVEL SECURITY', tbl);
  EXECUTE format('ALTER TABLE %s FORCE ROW LEVEL SECURITY', tbl);
  EXECUTE format('CREATE POLICY tenant_isolation ON %s USING (tenant_id = app.current_tenant()) '
                 'WITH CHECK (tenant_id = app.current_tenant())', tbl);
  EXECUTE format('CREATE TRIGGER touch BEFORE UPDATE ON %s FOR EACH ROW EXECUTE FUNCTION app.touch()', tbl);
  IF resource THEN
    EXECUTE format('CREATE TRIGGER guard_immutables BEFORE UPDATE ON %s FOR EACH ROW '
                   'EXECUTE FUNCTION app.guard_resource_immutables()', tbl);
  END IF;
END $$;
REVOKE EXECUTE ON FUNCTION app.setup_tenant_table(regclass, boolean) FROM PUBLIC, thelife_app, thelife_admin;

CREATE FUNCTION app.is_valid_timezone(tz text) RETURNS boolean LANGUAGE sql STABLE AS $$
  SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_timezone_names WHERE name = tz)
$$;

-- ---------------------------------------------------------------- tenants
CREATE TABLE tenants (
  id                   uuid PRIMARY KEY,
  name                 text NOT NULL CHECK (length(name) BETWEEN 1 AND 200),
  status               text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'suspended')),
  agent_writes_enabled boolean NOT NULL DEFAULT true,   -- kill switch (spec 5.3)
  settings             jsonb NOT NULL DEFAULT '{}'::jsonb,
  created_at           timestamptz NOT NULL DEFAULT now(),
  updated_at           timestamptz NOT NULL DEFAULT now(),
  version              integer NOT NULL DEFAULT 1
);
ALTER TABLE tenants ENABLE ROW LEVEL SECURITY;
ALTER TABLE tenants FORCE ROW LEVEL SECURITY;
-- A tenant sees only itself and cannot change its own status or kill switch (admin.* does that).
CREATE POLICY tenant_isolation ON tenants USING (id = app.current_tenant()) WITH CHECK (id = app.current_tenant());
CREATE TRIGGER touch BEFORE UPDATE ON tenants FOR EACH ROW EXECUTE FUNCTION app.touch();
REVOKE INSERT, UPDATE ON tenants FROM thelife_app;
GRANT UPDATE (name, settings) ON tenants TO thelife_app;

-- ---------------------------------------------------------------- users
CREATE TABLE users (
  id             uuid PRIMARY KEY,
  tenant_id      uuid NOT NULL REFERENCES tenants(id),
  email          citext NOT NULL UNIQUE CHECK (length(email) BETWEEN 3 AND 320),
  display_name   text NOT NULL CHECK (length(display_name) BETWEEN 1 AND 200),
  password_hash  text NOT NULL,
  role           text NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'tenant_admin')),
  timezone       text NOT NULL DEFAULT 'UTC',
  day_start_hour smallint NOT NULL DEFAULT 4 CHECK (day_start_hour BETWEEN 0 AND 12),
  workdays       text[] NOT NULL DEFAULT '{MO,TU,WE,TH,FR}'
                 CHECK (workdays <@ ARRAY['MO','TU','WE','TH','FR','SA','SU']),
  status         text NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'disabled')),
  created_at     timestamptz NOT NULL DEFAULT now(),
  updated_at     timestamptz NOT NULL DEFAULT now(),
  version        integer NOT NULL DEFAULT 1,
  UNIQUE (tenant_id, id)
);
SELECT app.setup_tenant_table('users', false);
-- The app may change profile fields and the password hash, never identity, role or status.
REVOKE INSERT, UPDATE ON users FROM thelife_app;
GRANT UPDATE (display_name, password_hash, timezone, day_start_hour, workdays) ON users TO thelife_app;

CREATE FUNCTION app.check_user_timezone() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NOT app.is_valid_timezone(NEW.timezone) THEN
    RAISE EXCEPTION 'invalid timezone: %', NEW.timezone USING ERRCODE = 'check_violation';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER check_timezone BEFORE INSERT OR UPDATE OF timezone ON users
  FOR EACH ROW EXECUTE FUNCTION app.check_user_timezone();

-- ---------------------------------------------------------------- devices (collector)
CREATE TABLE devices (
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
  user_id           uuid NOT NULL,
  name              text NOT NULL CHECK (length(name) BETWEEN 1 AND 200),
  platform          text NOT NULL CHECK (platform IN ('linux-x11')),
  hostname          text CHECK (length(hostname) <= 255),
  collector_version text CHECK (length(collector_version) <= 64),
  config            jsonb NOT NULL DEFAULT '{}'::jsonb,
  last_seen_at      timestamptz,
  revoked_at        timestamptz,
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, user_id) REFERENCES users (tenant_id, id)
);
SELECT app.setup_tenant_table('devices', true);

-- ---------------------------------------------------------------- credentials (tokens: sha256 only)
CREATE TABLE credentials (
  id           uuid PRIMARY KEY,
  tenant_id    uuid NOT NULL REFERENCES tenants(id),
  kind         credential_kind NOT NULL,
  user_id      uuid NOT NULL,
  device_id    uuid,
  name         text CHECK (length(name) <= 200),
  token_prefix text NOT NULL CHECK (length(token_prefix) = 8),
  token_hash   bytea NOT NULL UNIQUE CHECK (length(token_hash) = 32),
  scopes       text[] NOT NULL DEFAULT '{}',
  created_at   timestamptz NOT NULL DEFAULT now(),
  updated_at   timestamptz NOT NULL DEFAULT now(),
  version      integer NOT NULL DEFAULT 1,
  expires_at   timestamptz,
  last_used_at timestamptz,
  revoked_at   timestamptz,
  CHECK ((kind = 'device') = (device_id IS NOT NULL)),
  FOREIGN KEY (tenant_id, user_id) REFERENCES users (tenant_id, id),
  FOREIGN KEY (tenant_id, device_id) REFERENCES devices (tenant_id, id)
);
SELECT app.setup_tenant_table('credentials', false);
CREATE INDEX credentials_user_idx ON credentials (tenant_id, user_id) WHERE revoked_at IS NULL;
-- Tokens are revoked, never edited.
REVOKE UPDATE ON credentials FROM thelife_app;
GRANT UPDATE (revoked_at) ON credentials TO thelife_app;

-- ---------------------------------------------------------------- auth.* (pre-tenant lookups)
-- Runs as thelife_definer (BYPASSRLS). Fixed search_path; fully qualified names only.
CREATE FUNCTION auth.authenticate(p_hash bytea)
  RETURNS TABLE (credential_id uuid, tenant_id uuid, kind credential_kind, user_id uuid, device_id uuid,
                 scopes text[], expires_at timestamptz)
  LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
BEGIN
  -- Sliding expiry for sessions; written at most once a minute per token.
  UPDATE public.credentials c
     SET last_used_at = now(),
         expires_at = CASE WHEN c.kind = 'user_session' THEN now() + interval '30 days' ELSE c.expires_at END
   WHERE c.token_hash = p_hash AND c.revoked_at IS NULL AND (c.expires_at IS NULL OR c.expires_at > now())
     AND (c.last_used_at IS NULL OR c.last_used_at < now() - interval '1 minute');
  RETURN QUERY
    SELECT c.id, c.tenant_id, c.kind, c.user_id, c.device_id, c.scopes, c.expires_at
      FROM public.credentials c
      JOIN public.users u   ON u.id = c.user_id
      JOIN public.tenants t ON t.id = c.tenant_id
      LEFT JOIN public.devices d ON d.id = c.device_id
     WHERE c.token_hash = p_hash
       AND c.revoked_at IS NULL
       AND (c.expires_at IS NULL OR c.expires_at > now())
       AND u.status = 'active' AND t.status = 'active'
       AND (c.device_id IS NULL OR (d.revoked_at IS NULL AND d.deleted_at IS NULL));
END $$;

CREATE FUNCTION auth.find_login(p_email text)
  RETURNS TABLE (tenant_id uuid, user_id uuid, password_hash text)
  LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog AS $$
  SELECT u.tenant_id, u.id, u.password_hash
    FROM public.users u JOIN public.tenants t ON t.id = u.tenant_id
   WHERE u.email = p_email::public.citext AND u.status = 'active' AND t.status = 'active'
$$;

-- Assigning ownership requires the new owner to hold CREATE on the schema; the role cannot log in.
GRANT USAGE, CREATE ON SCHEMA auth TO thelife_definer;
ALTER FUNCTION auth.authenticate(bytea) OWNER TO thelife_definer;
ALTER FUNCTION auth.find_login(text) OWNER TO thelife_definer;
REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA auth FROM PUBLIC;
GRANT EXECUTE ON FUNCTION auth.authenticate(bytea), auth.find_login(text) TO thelife_app;

-- ---------------------------------------------------------------- admin.* (bootstrap CLI only)
CREATE SCHEMA admin AUTHORIZATION thelife_owner;
GRANT USAGE ON SCHEMA admin TO thelife_admin;

CREATE FUNCTION admin.create_tenant(p_id uuid, p_name text) RETURNS uuid
  LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog AS $$
  INSERT INTO public.tenants (id, name) VALUES (p_id, p_name) RETURNING id
$$;

CREATE FUNCTION admin.create_user(p_id uuid, p_tenant uuid, p_email text, p_display_name text,
                                  p_password_hash text, p_role text, p_timezone text) RETURNS uuid
  LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog AS $$
  INSERT INTO public.users (id, tenant_id, email, display_name, password_hash, role, timezone)
  VALUES (p_id, p_tenant, p_email::public.citext, p_display_name, p_password_hash, p_role, p_timezone)
  RETURNING id
$$;

CREATE FUNCTION admin.reset_password(p_email text, p_password_hash text) RETURNS uuid
  LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $$
DECLARE v_user uuid;
BEGIN
  UPDATE public.users SET password_hash = p_password_hash WHERE email = p_email::public.citext
    RETURNING id INTO v_user;
  IF v_user IS NULL THEN RAISE EXCEPTION 'no such user' USING ERRCODE = 'no_data_found'; END IF;
  UPDATE public.credentials SET revoked_at = now()
   WHERE user_id = v_user AND kind = 'user_session' AND revoked_at IS NULL;
  RETURN v_user;
END $$;

CREATE FUNCTION admin.set_agent_writes(p_tenant uuid, p_enabled boolean) RETURNS void
  LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $$
BEGIN
  UPDATE public.tenants SET agent_writes_enabled = p_enabled WHERE id = p_tenant;
  IF NOT FOUND THEN RAISE EXCEPTION 'no such tenant' USING ERRCODE = 'no_data_found'; END IF;
END $$;

CREATE FUNCTION admin.list_tenants()
  RETURNS TABLE (id uuid, name text, status text, agent_writes_enabled boolean, users bigint, created_at timestamptz)
  LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog AS $$
  SELECT t.id, t.name, t.status, t.agent_writes_enabled,
         (SELECT count(*) FROM public.users u WHERE u.tenant_id = t.id), t.created_at
    FROM public.tenants t ORDER BY t.created_at
$$;

GRANT USAGE, CREATE ON SCHEMA admin TO thelife_definer;
ALTER FUNCTION admin.create_tenant(uuid, text) OWNER TO thelife_definer;
ALTER FUNCTION admin.create_user(uuid, uuid, text, text, text, text, text) OWNER TO thelife_definer;
ALTER FUNCTION admin.reset_password(text, text) OWNER TO thelife_definer;
ALTER FUNCTION admin.set_agent_writes(uuid, boolean) OWNER TO thelife_definer;
ALTER FUNCTION admin.list_tenants() OWNER TO thelife_definer;
REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA admin FROM PUBLIC;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA admin TO thelife_admin;

-- thelife_definer reads and writes only what those functions touch.
GRANT USAGE ON SCHEMA public, app TO thelife_definer;
GRANT SELECT, INSERT, UPDATE ON tenants, users TO thelife_definer;
GRANT SELECT, UPDATE ON credentials TO thelife_definer;
GRANT SELECT ON devices TO thelife_definer;
GRANT EXECUTE ON FUNCTION app.touch(), app.check_user_timezone(), app.is_valid_timezone(text) TO thelife_definer;

-- migrate:down
DROP SCHEMA admin CASCADE;
DROP FUNCTION auth.find_login(text);
DROP FUNCTION auth.authenticate(bytea);
DROP TABLE credentials;
DROP TABLE devices;
DROP TABLE users;
DROP FUNCTION app.check_user_timezone();
DROP TABLE tenants;
DROP FUNCTION app.is_valid_timezone(text);
DROP FUNCTION app.setup_tenant_table(regclass, boolean);
DROP FUNCTION app.guard_resource_immutables();
DROP FUNCTION app.touch();
DROP TYPE credential_kind;
DROP TYPE origin_kind;
DROP TYPE principal_kind;
REVOKE USAGE ON SCHEMA public, app FROM thelife_definer;
REVOKE USAGE, CREATE ON SCHEMA auth FROM thelife_definer;
