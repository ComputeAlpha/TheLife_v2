#!/bin/bash
# Runs once, on first start of an empty data directory, as the postgres superuser.
# Creates the three roles from backend_lld.md §4.1, the database, and superuser-only extensions.
set -euo pipefail

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname postgres \
  -v owner_pw="$THELIFE_OWNER_PASSWORD" -v app_pw="$THELIFE_APP_PASSWORD" -v admin_pw="$THELIFE_ADMIN_PASSWORD" \
  -v db="$THELIFE_DB_NAME" <<'SQL'
CREATE ROLE thelife_owner LOGIN PASSWORD :'owner_pw';
CREATE ROLE thelife_app   LOGIN PASSWORD :'app_pw'   NOSUPERUSER NOBYPASSRLS NOCREATEDB NOCREATEROLE;
CREATE ROLE thelife_admin LOGIN PASSWORD :'admin_pw' NOSUPERUSER NOBYPASSRLS NOCREATEDB NOCREATEROLE;
-- Owns the SECURITY DEFINER functions for pre-tenant lookups (auth.*) and admin bootstrap (admin.*).
-- NOLOGIN: nobody connects as it; only those functions run with its rights (backend_lld.md §4.3).
CREATE ROLE thelife_definer NOLOGIN BYPASSRLS NOSUPERUSER NOCREATEDB NOCREATEROLE;
-- The migration role must be a member to assign function ownership to thelife_definer.
GRANT thelife_definer TO thelife_owner;
CREATE DATABASE :"db" OWNER thelife_owner;
REVOKE ALL ON DATABASE :"db" FROM PUBLIC;
GRANT CONNECT ON DATABASE :"db" TO thelife_app, thelife_admin;
GRANT CONNECT, CREATE ON DATABASE :"db" TO thelife_owner;
SQL

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$THELIFE_DB_NAME" <<'SQL'
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS citext;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
ALTER SCHEMA public OWNER TO thelife_owner;
SQL
