#!/usr/bin/env bash
# TheLife on this machine, always on (deploy/README.md). Separate from the dev stack (scripts/demo.sh, scripts/db.sh).
#   scripts/prod.sh init                  create ~/.config/thelife/server.env (0600), the KEK and the data directories
#   scripts/prod.sh build                 release binaries + web UI → build/deploy → container images
#   scripts/prod.sh up                    start or update the containers (migrations run first); restart on failure/boot
#   scripts/prod.sh update                build, then up (how a fix reaches the running deployment)
#   scripts/prod.sh down                  stop the containers (data stays)
#   scripts/prod.sh status                containers, readiness, embedder, collector, latest backup
#   scripts/prod.sh logs [SERVICE]        follow logs (backend, postgres, backup)
#   scripts/prod.sh account EMAIL "NAME" [TIMEZONE]   create the tenant and user; prints the password once
#   scripts/prod.sh backup                dump the database now into the backups directory
#   scripts/prod.sh install-embedder      GPU embedder as a user service (copies binary, ONNX Runtime and model)
#   scripts/prod.sh install-collector     X11 collector as a user service bound to the graphical session
set -euo pipefail
cd "$(dirname "$0")/.."

ENV_FILE="${THELIFE_ENV_FILE:-${XDG_CONFIG_HOME:-$HOME/.config}/thelife/server.env}"
DATA_DIR_DEFAULT="${XDG_DATA_HOME:-$HOME/.local/share}/thelife"
STATE_DIR_DEFAULT="${XDG_STATE_HOME:-$HOME/.local/state}/thelife"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"

load_env() {
  [[ -f "$ENV_FILE" ]] || { echo "no $ENV_FILE: run 'scripts/prod.sh init' first" >&2; exit 1; }
  set -a; # shellcheck disable=SC1090
  source "$ENV_FILE"; set +a
}
compose() { docker compose --env-file "$ENV_FILE" -f deploy/compose.yml "$@"; }

cmd_init() {
  umask 077
  mkdir -p "$(dirname "$ENV_FILE")"
  if [[ -f "$ENV_FILE" ]]; then
    echo "$ENV_FILE exists; kept"
  else
    pw() { openssl rand -hex 24; }
    cat > "$ENV_FILE" <<EOF
# TheLife deployment on this machine (scripts/prod.sh, deploy/README.md). Keep private (mode 0600).
THELIFE_PORT=8090
THELIFE_PG_PORT=55433
THELIFE_UID=$(id -u)
THELIFE_GID=$(id -g)
TZ=$(timedatectl show -p Timezone --value 2>/dev/null || echo UTC)
THELIFE_DATA_DIR=$DATA_DIR_DEFAULT
THELIFE_STATE_DIR=$STATE_DIR_DEFAULT
# Database passwords, generated once. The roles get them when the volume is first created; changing them here
# later does not change the database.
PG_SUPERUSER_PASSWORD=$(pw)
PG_OWNER_PASSWORD=$(pw)
PG_APP_PASSWORD=$(pw)
PG_ADMIN_PASSWORD=$(pw)
# Telegram bot token from @BotFather (empty: Telegram off). Only one backend may use a token at a time:
# don't export the same token for scripts/demo.sh while this runs.
THELIFE_TELEGRAM_BOT_TOKEN=
# Ollama web search key (https://ollama.com/settings/keys; empty: web search off).
OLLAMA_API_KEY=
EOF
    echo "created $ENV_FILE (add the Telegram bot token and, optionally, OLLAMA_API_KEY)"
  fi
  load_env
  mkdir -p "$THELIFE_DATA_DIR/backups" "$THELIFE_STATE_DIR/run"
  chmod 700 "$THELIFE_DATA_DIR" "$THELIFE_STATE_DIR" "$THELIFE_STATE_DIR/run"
  if [[ ! -s "$THELIFE_DATA_DIR/kek" ]]; then
    head -c 32 /dev/urandom | base64 > "$THELIFE_DATA_DIR/kek"
    echo "created the encryption key $THELIFE_DATA_DIR/kek: back it up with server.env; without it the"
    echo "encrypted fields (notes, descriptions, detections) of the database and of every backup are unreadable"
  fi
}

cmd_build() {
  [[ -d build/release ]] || VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}" cmake --preset release
  cmake --build build/release -j"$(nproc)" --target thelife-backend thelife-admin thelife-collector thelife-embedder
  (cd frontend && { [[ -d node_modules ]] || npm ci --no-audit --no-fund; } && npm run build)
  local s=build/deploy
  rm -rf "$s" && mkdir -p "$s/bin" "$s/config"
  cp build/release/backend/thelife-backend build/release/backend/thelife-admin "$s/bin/"
  strip --strip-debug "$s/bin/"*
  cp -r frontend/dist "$s/web"
  cp config/llm.json "$s/config/"
  cp -r backend/migrations "$s/migrations"
  cp deploy/Dockerfile "$s/"
  { git describe --always --dirty 2>/dev/null || echo unknown; date -u +%FT%TZ; } > "$s/VERSION"
  load_env
  compose build
  echo "built $(head -1 "$s/VERSION")"
}

cmd_up() {
  load_env
  [[ -s "$THELIFE_DATA_DIR/kek" ]] || { echo "no KEK: run 'scripts/prod.sh init'" >&2; exit 1; }
  docker image inspect thelife-backend:latest >/dev/null 2>&1 || { echo "no image: run 'scripts/prod.sh build'" >&2; exit 1; }
  compose up -d --wait --no-build postgres backend backup
  echo "TheLife  http://localhost:${THELIFE_PORT}   (scripts/prod.sh status | logs)"
}

cmd_status() {
  load_env
  compose ps --format 'table {{.Service}}\t{{.Status}}'
  if curl -sf "http://127.0.0.1:$THELIFE_PORT/readyz" >/dev/null; then
    echo "backend  ready  $(curl -sf "http://127.0.0.1:$THELIFE_PORT/healthz")"
  else
    echo "backend  NOT READY (scripts/prod.sh logs backend)"
  fi
  [[ -S "$THELIFE_STATE_DIR/run/embedder.sock" ]] && echo "embedder socket present" || echo "embedder not running (keyword search only)"
  printf 'services embedder=%s collector=%s\n' "$(systemctl --user is-active thelife-embedder 2>/dev/null || true)" \
    "$(systemctl --user is-active thelife-collector 2>/dev/null || true)"
  local last; last="$(ls -1t "$THELIFE_DATA_DIR/backups"/thelife-*.dump 2>/dev/null | head -1 || true)"
  echo "backup   ${last:-none yet}"
}

cmd_account() {
  local email="${1:?usage: scripts/prod.sh account EMAIL \"NAME\" [TIMEZONE]}"
  local name="${2:?usage: scripts/prod.sh account EMAIL \"NAME\" [TIMEZONE]}"
  local tz="${3:-Asia/Kolkata}"
  load_env
  local admin="postgresql://thelife_admin:${PG_ADMIN_PASSWORD}@127.0.0.1:${THELIFE_PG_PORT}/thelife"
  local tenant
  tenant="$(compose exec -T -e THELIFE_ADMIN_DB_URL="$admin" backend thelife-admin tenant create --name "$name" \
    | grep -oE '[0-9a-f-]{36}')"
  compose exec -T -e THELIFE_ADMIN_DB_URL="$admin" backend thelife-admin user create --tenant "$tenant" \
    --email "$email" --name "$name" --timezone "$tz"
}

cmd_backup() {
  load_env
  local f="$THELIFE_DATA_DIR/backups/thelife-$(date +%Y%m%d-%H%M).dump"
  (umask 077 && compose exec -T postgres pg_dump -U postgres -d thelife -Fc > "$f.part") && mv "$f.part" "$f"
  echo "backup: $f ($(du -h "$f" | cut -f1))"
}

cmd_install_embedder() {
  load_env
  local bin=build/release/backend/embedder/thelife-embedder
  local ort=build/release/_deps/onnxruntime-linux-x64-gpu-1.26.0/lib
  [[ -x "$bin" ]] || { echo "no $bin: run 'scripts/prod.sh build'" >&2; exit 1; }
  local lib="$HOME/.local/lib/thelife/embedder"
  local models="$THELIFE_DATA_DIR/models"
  local src_model="${THELIFE_EMBED_MODEL_DIR:-spikes/embed/models/bge-m3-fp16}"
  local src_spm="${THELIFE_EMBED_SPM:-spikes/embed/models/bge-m3/onnx/sentencepiece.bpe.model}"
  mkdir -p "$lib" "$models/bge-m3-fp16" "$UNIT_DIR"
  install -m755 "$bin" "$lib/"
  cp -P "$ort"/libonnxruntime.so* "$ort"/libonnxruntime_providers_shared.so "$ort"/libonnxruntime_providers_cuda.so "$lib/"
  [[ -f "$models/bge-m3-fp16/model.onnx.data" ]] || cp "$src_model"/model.onnx "$src_model"/model.onnx.data "$models/bge-m3-fp16/"
  [[ -f "$models/sentencepiece.bpe.model" ]] || cp "$src_spm" "$models/"
  install -m644 deploy/thelife-embedder.service "$UNIT_DIR/"
  systemctl --user daemon-reload
  systemctl --user enable thelife-embedder
  systemctl --user restart thelife-embedder
  echo "embedder installed; 'journalctl --user -u thelife-embedder' shows its log"
}

cmd_install_collector() {
  load_env
  local bin=build/release/collector/thelife-collector
  [[ -x "$bin" ]] || { echo "no $bin: run 'scripts/prod.sh build'" >&2; exit 1; }
  mkdir -p "$HOME/.local/bin" "$UNIT_DIR"
  # Replace the binary only while the service is stopped (it is busy while running).
  local was_active=0
  systemctl --user is-active -q thelife-collector 2>/dev/null && { was_active=1; systemctl --user stop thelife-collector; }
  install -m755 "$bin" "$HOME/.local/bin/thelife-collector"
  sed "s|^ExecStart=.*|ExecStart=%h/.local/bin/thelife-collector run|" \
    collector/packaging/systemd/thelife-collector.service > "$UNIT_DIR/thelife-collector.service"
  systemctl --user daemon-reload
  local cfg="${XDG_CONFIG_HOME:-$HOME/.config}/thelife-collector"
  if [[ -s "$cfg/device.token" ]] && grep -q "127.0.0.1:${THELIFE_PORT}" "$cfg/config.toml" 2>/dev/null; then
    systemctl --user enable thelife-collector
    systemctl --user restart thelife-collector
    echo "collector running against http://127.0.0.1:${THELIFE_PORT}"
  else
    # Not enabled yet: started now, it would report to wherever it was enrolled before.
    systemctl --user disable -q thelife-collector 2>/dev/null || true
    [[ $was_active == 1 ]] && echo "(it was running for another backend; stopped until enrolled here)"
    cat <<EOF
collector installed, not enrolled with this deployment yet:
  1. Settings → Connections → Devices → Register in http://localhost:${THELIFE_PORT}, copy the tld_ token
  2. ~/.local/bin/thelife-collector enroll --url http://127.0.0.1:${THELIFE_PORT} --web-ui http://localhost:${THELIFE_PORT}
     (paste the token when asked)
  3. systemctl --user enable --now thelife-collector
EOF
  fi
}

case "${1:-}" in
  init) cmd_init ;;
  build) cmd_build ;;
  up) cmd_up ;;
  update) cmd_build; cmd_up ;;
  down) load_env; compose stop ;;
  status) cmd_status ;;
  logs) load_env; compose logs -f --tail=100 "${@:2}" ;;
  account) shift; cmd_account "$@" ;;
  backup) cmd_backup ;;
  install-embedder) cmd_install_embedder ;;
  install-collector) cmd_install_collector ;;
  *) sed -n '2,14p' "$0"; exit 2 ;;
esac
