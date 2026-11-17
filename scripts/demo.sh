#!/usr/bin/env bash
# Run TheLife on this machine (dev defaults, 127.0.0.1 only): the backend serves the web UI on the same port.
#   scripts/demo.sh account EMAIL "NAME" [TIMEZONE]   create a tenant and user; prints the password once
#   scripts/demo.sh up                              Postgres + migrations + backend and web UI (:8080)
#   scripts/demo.sh collector                       build and run the X11 collector against it (enrolls first;
#                                                   paste the token from Settings → Connections → Devices → Register)
# Ctrl-C stops it. Logs: build/demo-backend.log. Port: THELIFE_LISTEN_PORT.
# The Flask demo UI was retired on 2026-11-17 (kept for reference under spikes/demo-ui; not started any more).
# LLM: config/llm.json (local Ollama, glm-5.3-flash:cloud). DEMO_FAKE_LLM=1 uses docker/fake-llm instead (no quota).
# Background jobs (routines, digests, nudges, ingest) run only for the tenant in build/demo-tenant, written by
# `account`, so leftover test tenants in the dev database never call the LLM. THELIFE_JOBS_TENANT overrides it.
# Telegram: export THELIFE_TELEGRAM_BOT_TOKEN (from @BotFather) before `up`; link the chat in Settings → Connections.
# Web search: export OLLAMA_API_KEY (https://ollama.com/settings/keys) before starting; without it the tool is off.
# Embeddings: thelife-embedder starts when built and the model files exist (THELIFE_EMBED_MODEL, THELIFE_EMBED_SPM;
# default: the Stage 0 spike download under spikes/embed/models). DEMO_EMBEDDER=0 skips it (keyword search only).
set -euo pipefail
cd "$(dirname "$0")/.."

PG_PORT="${THELIFE_PG_PORT:-55432}"
API_PORT="${THELIFE_LISTEN_PORT:-8080}"
BIN=build/dev/backend
export THELIFE_DB_URL="${THELIFE_DB_URL:-postgresql://thelife_app:${THELIFE_APP_PASSWORD:-app-dev-only}@127.0.0.1:${PG_PORT}/thelife}"
export THELIFE_LLM_CONFIG="${THELIFE_LLM_CONFIG:-config/llm.json}"
export THELIFE_ADMIN_DB_URL="${THELIFE_ADMIN_DB_URL:-postgresql://thelife_admin:${THELIFE_ADMIN_PASSWORD:-admin-dev-only}@127.0.0.1:${PG_PORT}/thelife}"
# Encryption key (KEK) of the dev database: created once under build/. Deleting it makes the encrypted fields of
# the dev database unreadable (notes, descriptions, detection text).
if [[ -z "${THELIFE_KEK:-}" ]]; then
  export THELIFE_KEK_FILE="${THELIFE_KEK_FILE:-build/dev-kek}"
  if [[ ! -s "$THELIFE_KEK_FILE" ]]; then
    mkdir -p "$(dirname "$THELIFE_KEK_FILE")"
    (umask 077 && head -c 32 /dev/urandom | base64 > "$THELIFE_KEK_FILE")
    echo "created a dev encryption key in $THELIFE_KEK_FILE (keep it with the dev database)" >&2
  fi
fi

database() {
  scripts/db.sh up >/dev/null
  scripts/db.sh migrate
}

build() {
  [[ -d build/dev ]] || cmake --preset dev
  local targets=(thelife-backend thelife-admin thelife-collector)
  [[ "${DEMO_EMBEDDER:-1}" == 1 ]] && targets+=(thelife-embedder)
  cmake --build build/dev -j"$(nproc)" --target "${targets[@]}"
  # The web UI (Stage 5), served by the backend from frontend/dist.
  if command -v npm >/dev/null; then
    (cd frontend && { [[ -d node_modules ]] || npm ci --no-audit --no-fund; } && npm run build >/dev/null) \
      || echo "web UI build failed (cd frontend && npm run build)" >&2
  else
    echo "npm not found: the web UI is not built (Node 22 needed)" >&2
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
    echo "$tenant" > build/demo-tenant
    echo "background jobs of 'scripts/demo.sh up' will run for this tenant only (build/demo-tenant)"
    ;;
  collector)
    cmake --build build/dev -j"$(nproc)" --target thelife-collector >/dev/null
    col=build/dev/collector/thelife-collector
    if [[ ! -s "${XDG_CONFIG_HOME:-$HOME/.config}/thelife-collector/device.token" ]]; then
      echo "Register a device in the web UI (Settings → Connections → Devices → Register) and paste its tld_ token here."
      "$col" enroll --url "http://127.0.0.1:$API_PORT" --web-ui "http://127.0.0.1:$API_PORT"
    fi
    echo "collector running on $DISPLAY (Ctrl-C stops it; 'thelife-collector status' / 'pause 30m' in another terminal)"
    exec "$col" run
    ;;
  up)
    database
    build
    fake=""
    if [[ "${DEMO_FAKE_LLM:-0}" == 1 ]]; then
      python3 -I docker/fake-llm/fake_llm.py --port "${DEMO_FAKE_LLM_PORT:-11435}" > build/demo-fake-llm.log 2>&1 &
      fake=$!
      export THELIFE_LLM_BASE_URL="http://127.0.0.1:${DEMO_FAKE_LLM_PORT:-11435}/v1"
    fi
    embedder=""
    model="${THELIFE_EMBED_MODEL:-spikes/embed/models/bge-m3-fp16/model.onnx}"
    spm="${THELIFE_EMBED_SPM:-spikes/embed/models/bge-m3/onnx/sentencepiece.bpe.model}"
    if [[ "${DEMO_EMBEDDER:-1}" == 1 && -x "$BIN/embedder/thelife-embedder" && -f "$model" && -f "$spm" ]]; then
      export THELIFE_EMBEDDER_SOCKET="${XDG_RUNTIME_DIR:-/tmp}/thelife/embedder.sock"
      "$BIN/embedder/thelife-embedder" --model "$model" --spm "$spm" --socket "$THELIFE_EMBEDDER_SOCKET" \
        > build/demo-embedder.log 2>&1 &
      embedder=$!
      for _ in $(seq 1 60); do [[ -S "$THELIFE_EMBEDDER_SOCKET" ]] && break; sleep 0.5; done
      [[ -S "$THELIFE_EMBEDDER_SOCKET" ]] || echo "embedder did not start; see build/demo-embedder.log" >&2
    else
      echo "embedder not started (keyword search only)" >&2
    fi
    if [[ -z "${THELIFE_JOBS_TENANT:-}" ]]; then
      if [[ -s build/demo-tenant ]]; then
        export THELIFE_JOBS_TENANT="$(cat build/demo-tenant)"
      else
        echo "no build/demo-tenant (run 'scripts/demo.sh account' first): background jobs are off" >&2
        export THELIFE_JOBS=off
      fi
    fi
    # The React web UI (Stage 5) is served by the backend itself once it is built (npm run build in frontend/).
    if [[ -z "${THELIFE_WEB_ROOT:-}" && -f frontend/dist/index.html ]]; then export THELIFE_WEB_ROOT="$PWD/frontend/dist"; fi
    THELIFE_LISTEN_PORT="$API_PORT" "$BIN/thelife-backend" > build/demo-backend.log 2>&1 &
    backend=$!
    trap 'kill "$backend" $fake $embedder 2>/dev/null || true' EXIT INT TERM
    for _ in $(seq 1 30); do
      curl -sf "http://127.0.0.1:$API_PORT/readyz" >/dev/null && break
      kill -0 "$backend" 2>/dev/null || { echo "backend exited; see build/demo-backend.log" >&2; exit 1; }
      sleep 0.5
    done
    curl -sf "http://127.0.0.1:$API_PORT/readyz" >/dev/null || { echo "backend not ready; see build/demo-backend.log" >&2; exit 1; }
    echo "backend  http://127.0.0.1:$API_PORT   (log: build/demo-backend.log)"
    [[ -n "$embedder" ]] && echo "embedder $THELIFE_EMBEDDER_SOCKET   (log: build/demo-embedder.log)"
    [[ -n "${OLLAMA_API_KEY:-}" ]] && echo "web search on (OLLAMA_API_KEY)" || echo "web search off (no OLLAMA_API_KEY)"
    [[ -n "${THELIFE_TELEGRAM_BOT_TOKEN:-}" ]] && echo "telegram on (link your chat in Settings)" || echo "telegram off (no THELIFE_TELEGRAM_BOT_TOKEN)"
    [[ -n "${THELIFE_JOBS_TENANT:-}" ]] && echo "jobs     tenant $THELIFE_JOBS_TENANT only"
    [[ -n "${THELIFE_WEB_ROOT:-}" ]] && echo "web UI   http://127.0.0.1:$API_PORT   ($THELIFE_WEB_ROOT)"
    [[ -n "${THELIFE_WEB_ROOT:-}" ]] || echo "web UI not built: run 'npm ci && npm run build' in frontend/ (Node 22)" >&2
    echo "Ctrl-C stops it."
    wait "$backend"
    ;;
  *)
    sed -n '2,5p' "$0"; exit 2 ;;
esac
