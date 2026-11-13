#!/usr/bin/env bash
# Local database helper. Runs dbmate from its container image, so nothing is installed on the host.
#   scripts/db.sh up        start postgres (compose) and wait for health
#   scripts/db.sh migrate   apply pending migrations as thelife_owner
#   scripts/db.sh rollback  roll back the latest migration
#   scripts/db.sh status    show migration status
#   scripts/db.sh reset     DESTROY the local database volume and start fresh
#   scripts/db.sh urls      print connection URLs (dev defaults)
#   scripts/db.sh test      create (if missing) and migrate the separate test database, thelife_test, and print
#                           the THELIFE_TEST_* URLs. Tests create thousands of tenants and run jobs; keeping them
#                           out of the dev database keeps the demo backend from processing (and paying for) them.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${THELIFE_PG_PORT:-55432}"
DB="${THELIFE_DB_NAME:-thelife}"
[[ "${1:-}" == "test" ]] && DB=thelife_test
OWNER_URL="postgres://thelife_owner:${THELIFE_OWNER_PASSWORD:-owner-dev-only}@127.0.0.1:${PORT}/${DB}?sslmode=disable"
APP_URL="postgresql://thelife_app:${THELIFE_APP_PASSWORD:-app-dev-only}@127.0.0.1:${PORT}/${DB}"
ADMIN_URL="postgresql://thelife_admin:${THELIFE_ADMIN_PASSWORD:-admin-dev-only}@127.0.0.1:${PORT}/${DB}"
DBMATE_IMAGE="ghcr.io/amacneil/dbmate:2.28"

dbmate() {
  docker run --rm --network host -v "$PWD/backend/migrations:/db/migrations:ro" \
    -e DATABASE_URL="$OWNER_URL" "$DBMATE_IMAGE" \
    --migrations-dir /db/migrations --no-dump-schema --wait "$@"
}

case "${1:-}" in
  up)       docker compose -f docker/compose.yml up -d --wait ;;
  migrate)  dbmate up ;;
  rollback) dbmate rollback ;;
  status)   dbmate status ;;
  reset)    docker compose -f docker/compose.yml down -v && docker compose -f docker/compose.yml up -d --wait ;;
  urls)     echo "owner: $OWNER_URL"; echo "app:   $APP_URL" ;;
  test)
    # Same roles as the dev database (created by docker/postgres/init); only the database is new.
    if ! docker compose -f docker/compose.yml exec -T postgres psql -U postgres -tAc \
         "SELECT 1 FROM pg_database WHERE datname = '$DB'" | grep -q 1; then
      docker compose -f docker/compose.yml exec -T postgres psql -v ON_ERROR_STOP=1 -U postgres -q <<SQL
CREATE DATABASE $DB OWNER thelife_owner;
REVOKE ALL ON DATABASE $DB FROM PUBLIC;
GRANT CONNECT ON DATABASE $DB TO thelife_app, thelife_admin;
GRANT CONNECT, CREATE ON DATABASE $DB TO thelife_owner;
\\c $DB
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS citext;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
ALTER SCHEMA public OWNER TO thelife_owner;
SQL
    fi
    dbmate up >&2
    echo "export THELIFE_TEST_DB_URL=$APP_URL"
    echo "export THELIFE_TEST_ADMIN_DB_URL=$ADMIN_URL" ;;
  *)        sed -n '2,9p' "$0"; exit 2 ;;
esac
