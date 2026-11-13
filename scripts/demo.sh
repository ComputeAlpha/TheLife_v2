#!/usr/bin/env bash
# Run the backend and the demo UI together on this machine (dev defaults, 127.0.0.1 only).
#   scripts/demo.sh account EMAIL "NAME" [TIMEZONE]   create a tenant and user; prints the password once
#   scripts/demo.sh up                              Postgres + migrations + backend (:8080) + demo UI (:5000)
# Ctrl-C stops both. Logs: build/demo-backend.log. Ports: THELIFE_LISTEN_PORT, DEMO_PORT.
# LLM: config/llm.json (local Ollama, glm-5.3-flash:cloud). DEMO_FAKE_LLM=1 uses docker/fake-llm instead (no quota).
set -euo pipefail
cd "$(dirname "$0")/.."

PG_PORT="${THELIFE_PG_PORT:-55432}"
API_PORT="${THELIFE_LISTEN_PORT:-8080}"
UI_PORT="${DEMO_PORT:-5000}"
BIN=build/dev/backend
export THELIFE_DB_URL="${THELIFE_DB_URL:-postgresql://thelife_app:${THELIFE_APP_PASSWORD:-app-dev-only}@127.0.0.1:${PG_PORT}/thelife}"
export THELIFE_LLM_CONFIG="${THELIFE_LLM_CONFIG:-config/llm.json}"
export THELIFE_ADMIN_DB_URL="${THELIFE_ADMIN_DB_URL:-postgresql://thelife_admin:${THELIFE_ADMIN_PASSWORD:-admin-dev-only}@127.0.0.1:${PG_PORT}/thelife}"

database() {
  scripts/db.sh up >/dev/null
  scripts/db.sh migrate
}

build() {
  [[ -d build/dev ]] || cmake --preset dev
  cmake --build build/dev -j"$(nproc)" --target thelife-backend thelife-admin
}

venv() {
  if [[ ! -x build/demo-venv/bin/python ]]; then
    python3 -m venv build/demo-venv
    env -u PYTHONPATH build/demo-venv/bin/pip install -q -r demo-ui/requirements.txt
  fi
}

case "${1:-}" in
  account)
    email="${2:?usage: scripts/demo.sh account EMAIL \"NAME\" [TIMEZONE]}"
    name="${3:?usage: scripts/demo.sh account EMAIL \"NAME\" [TIMEZONE]}"
    tz="${4:-Asia/Kolkata}"
    database >/dev/null
    build >/dev/null
    tenant="$("$BIN/thelife-admin" tenant create --name "$name" | grep -oE '[0-9a-f-]{36}')"
    "$BIN/thelife-admin" user create --tenant "$tenant" --email "$email" --name "$name" --timezone "$tz"
    ;;
  up)
    database
    build
    venv
    fake=""
    if [[ "${DEMO_FAKE_LLM:-0}" == 1 ]]; then
      python3 -I docker/fake-llm/fake_llm.py --port "${DEMO_FAKE_LLM_PORT:-11435}" > build/demo-fake-llm.log 2>&1 &
      fake=$!
      export THELIFE_LLM_BASE_URL="http://127.0.0.1:${DEMO_FAKE_LLM_PORT:-11435}/v1"
    fi
    THELIFE_LISTEN_PORT="$API_PORT" "$BIN/thelife-backend" > build/demo-backend.log 2>&1 &
    backend=$!
    trap 'kill "$backend" $fake 2>/dev/null || true' EXIT INT TERM
    for _ in $(seq 1 30); do
      curl -sf "http://127.0.0.1:$API_PORT/readyz" >/dev/null && break
      kill -0 "$backend" 2>/dev/null || { echo "backend exited; see build/demo-backend.log" >&2; exit 1; }
      sleep 0.5
    done
    curl -sf "http://127.0.0.1:$API_PORT/readyz" >/dev/null || { echo "backend not ready; see build/demo-backend.log" >&2; exit 1; }
    echo "backend  http://127.0.0.1:$API_PORT   (log: build/demo-backend.log)"
    echo "demo UI  http://127.0.0.1:$UI_PORT   (Ctrl-C stops both)"
    cd demo-ui
    env -u PYTHONPATH THELIFE_API_URL="http://127.0.0.1:$API_PORT" DEMO_PORT="$UI_PORT" \
      ../build/demo-venv/bin/python -m demo_ui
    ;;
  *)
    sed -n '2,5p' "$0"; exit 2 ;;
esac
