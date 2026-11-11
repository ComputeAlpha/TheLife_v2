#!/usr/bin/env bash
# Local database helper. Runs dbmate from its container image, so nothing is installed on the host.
#   scripts/db.sh up        start postgres (compose) and wait for health
#   scripts/db.sh migrate   apply pending migrations as thelife_owner
#   scripts/db.sh rollback  roll back the latest migration
#   scripts/db.sh status    show migration status
#   scripts/db.sh reset     DESTROY the local database volume and start fresh
#   scripts/db.sh urls      print connection URLs (dev defaults)
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${THELIFE_PG_PORT:-55432}"
OWNER_URL="postgres://thelife_owner:${THELIFE_OWNER_PASSWORD:-owner-dev-only}@127.0.0.1:${PORT}/thelife?sslmode=disable"
APP_URL="postgresql://thelife_app:${THELIFE_APP_PASSWORD:-app-dev-only}@127.0.0.1:${PORT}/thelife"
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
  *)        sed -n '2,9p' "$0"; exit 2 ;;
esac
