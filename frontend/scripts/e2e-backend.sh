#!/usr/bin/env bash
# Runs the real backend for the Playwright tests (frontend_lld.md §9): the test database, the built web UI on the
# same origin, background jobs off. Needs `eval "$(scripts/db.sh test)"` and `npm run build` first.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${THELIFE_TEST_DB_URL:?run: eval \"\$(scripts/db.sh test)\"}"
[[ -f frontend/dist/index.html ]] || { echo "build the frontend first (npm run build)" >&2; exit 1; }
exec env -i PATH="$PATH" HOME="$HOME" \
  THELIFE_DB_URL="$THELIFE_TEST_DB_URL" \
  THELIFE_KEK="dGhlbGlmZS10ZXN0LWtlay1ub3QtYS1zZWNyZXQhISE=" \
  THELIFE_LISTEN_ADDR=127.0.0.1 THELIFE_LISTEN_PORT="${E2E_PORT:-18380}" \
  THELIFE_JOBS=off THELIFE_PROACTIVE=off \
  THELIFE_LLM_STATE_SCOPE="e2e$$" \
  ${E2E_LLM_BASE_URL:+THELIFE_LLM_BASE_URL="$E2E_LLM_BASE_URL"} \
  THELIFE_WEB_ROOT="$PWD/frontend/dist" \
  "${E2E_BACKEND_BIN:-build/dev/backend/thelife-backend}"
