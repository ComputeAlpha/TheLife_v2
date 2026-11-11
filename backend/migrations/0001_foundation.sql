-- migrate:up
-- Foundation: schemas, privileges and the tenant-context helper used by every RLS policy (backend_lld.md §4).
-- Runs as thelife_owner. Roles and extensions are created by the bootstrap script (docker/postgres/init).

CREATE SCHEMA auth AUTHORIZATION thelife_owner;   -- SECURITY DEFINER pre-tenant lookups (§4.3)
CREATE SCHEMA app  AUTHORIZATION thelife_owner;   -- helper functions

GRANT USAGE ON SCHEMA public, app TO thelife_app, thelife_admin;
GRANT USAGE ON SCHEMA auth TO thelife_app;

-- Every table the owner creates from now on is usable by the app role; RLS still applies to it.
ALTER DEFAULT PRIVILEGES FOR ROLE thelife_owner IN SCHEMA public
  GRANT SELECT, INSERT, UPDATE ON TABLES TO thelife_app;
ALTER DEFAULT PRIVILEGES FOR ROLE thelife_owner IN SCHEMA public
  GRANT USAGE, SELECT ON SEQUENCES TO thelife_app;
ALTER DEFAULT PRIVILEGES FOR ROLE thelife_owner IN SCHEMA app
  GRANT EXECUTE ON FUNCTIONS TO thelife_app, thelife_admin;
-- DELETE is granted per table where soft delete is not enough (none so far).

-- NULL when no tenant context is set, so RLS predicates evaluate to NULL and hide every row.
CREATE FUNCTION app.current_tenant() RETURNS uuid
  LANGUAGE sql STABLE PARALLEL SAFE
  AS $$ SELECT nullif(current_setting('app.tenant_id', true), '')::uuid $$;

-- dbmate's bookkeeping table must be readable for /readyz.
GRANT SELECT ON public.schema_migrations TO thelife_app;

-- migrate:down
REVOKE SELECT ON public.schema_migrations FROM thelife_app;
DROP FUNCTION app.current_tenant();
ALTER DEFAULT PRIVILEGES FOR ROLE thelife_owner IN SCHEMA app REVOKE EXECUTE ON FUNCTIONS FROM thelife_app, thelife_admin;
ALTER DEFAULT PRIVILEGES FOR ROLE thelife_owner IN SCHEMA public REVOKE USAGE, SELECT ON SEQUENCES FROM thelife_app;
ALTER DEFAULT PRIVILEGES FOR ROLE thelife_owner IN SCHEMA public REVOKE SELECT, INSERT, UPDATE ON TABLES FROM thelife_app;
DROP SCHEMA app;
DROP SCHEMA auth;
