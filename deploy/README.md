# TheLife on this machine

The always-on deployment for daily use, managed with `scripts/prod.sh`. It is separate from the dev stack
(`scripts/demo.sh`, `scripts/db.sh`, `docker/compose.yml`). It has its own compose project (`thelife-prod`),
Postgres volume, port and encryption key, so tests and dev resets never touch real data.

| Part | Runs as | Restarts |
|---|---|---|
| Postgres 17 + pgvector (`127.0.0.1:55433`) | container | `unless-stopped`, comes back after a reboot |
| Backend + web UI (`http://localhost:8090`) | container, host network, read-only, your uid | `unless-stopped`; health check on `/readyz` |
| Migrations | one-shot container before the backend | on every `prod.sh up` |
| Daily backup (`pg_dump -Fc`, 14 days kept) | container | `unless-stopped` |
| Embedder (bge-m3 on the GPU) | systemd user service | `Restart=always`, starts at login |
| Collector (X11) | systemd user service, bound to the graphical session | `Restart=on-failure` |

The embedder runs on the host because Docker here has no GPU runtime. The backend reaches it through
`~/.local/state/thelife/run/embedder.sock`. When the embedder is down, retrieval falls back to keyword search.

## Files

| Path | What |
|---|---|
| `~/.config/thelife/server.env` (0600) | ports, uid, database passwords (generated), Telegram bot token, Ollama web-search key |
| `~/.local/share/thelife/kek` (0600) | encryption key of the database's sealed fields |
| `~/.local/share/thelife/backups/` | daily dumps |
| `~/.local/share/thelife/models/` | embedder model (copied from `spikes/embed/models`) |
| `~/.local/lib/thelife/embedder/`, `~/.local/bin/thelife-collector` | installed binaries |
| `~/.config/systemd/user/thelife-{embedder,collector}.service` | user services |
| Docker volume `thelife-prod_pgdata` | the database |

**Back up `server.env` and `kek` somewhere off this machine** (a password manager is fine). A dump is useless
without the key: notes, descriptions and detections are encrypted. The dumps also sit on this disk; copy them
elsewhere if the disk itself is a concern.

## First setup

```bash
scripts/prod.sh init                 # env file, key, directories
# put THELIFE_TELEGRAM_BOT_TOKEN (and optionally OLLAMA_API_KEY) into ~/.config/thelife/server.env
scripts/prod.sh build                # release binaries, web UI, images
scripts/prod.sh up
scripts/prod.sh install-embedder
scripts/prod.sh account EMAIL "NAME" Asia/Kolkata    # prints the password once
# sign in at http://localhost:8090, then Settings → Connections:
#   Telegram → link your chat
#   Devices → Register → copy the tld_ token, then:
~/.local/bin/thelife-collector enroll --url http://127.0.0.1:8090 --web-ui http://localhost:8090
scripts/prod.sh install-collector    # enables the service once it is enrolled here
```

Optional: `loginctl enable-linger $USER` starts the embedder at boot rather than at login. The containers
start at boot either way.

## Everyday

- `scripts/prod.sh status`: containers, readiness, embedder, collector, latest backup.
- `scripts/prod.sh logs [backend]`: logs. `journalctl --user -u thelife-embedder -u thelife-collector` for the host services.
- `scripts/prod.sh update`: deploy a fix (build, then recreate the backend; migrations run first). Data is kept.
- `scripts/prod.sh backup`: dump now (do this before an update that has migrations).
- `scripts/prod.sh down`: stop the containers. `up` starts them again.
- Changed `server.env` (for example, added the Telegram token)? Run `scripts/prod.sh up`.

Use `localhost` for this deployment and `127.0.0.1` for the dev demo. Cookies are per host, not per port, so
two backends on the same host name would sign each other out.

Only one backend may poll a Telegram bot token. Don't export the same token for `scripts/demo.sh`. The dev demo
also uses the same Ollama account, so stop it (Ctrl-C) when it isn't needed.

## Restore

Tested on a throwaway stack: the roles exist from the first start, and the dump restores into a fresh database.

```bash
scripts/prod.sh down
docker compose -p thelife-prod -f deploy/compose.yml --env-file ~/.config/thelife/server.env up -d postgres
docker exec -i thelife-prod-postgres-1 psql -U postgres -c "DROP DATABASE thelife WITH (FORCE)" \
  -c "CREATE DATABASE thelife OWNER thelife_owner" -c "REVOKE ALL ON DATABASE thelife FROM PUBLIC" \
  -c "GRANT CONNECT ON DATABASE thelife TO thelife_app, thelife_admin" \
  -c "GRANT CONNECT, CREATE ON DATABASE thelife TO thelife_owner"
docker exec -i thelife-prod-postgres-1 pg_restore -U postgres -d thelife --exit-on-error < ~/.local/share/thelife/backups/thelife-YYYYMMDD-HHMM.dump
scripts/prod.sh up                   # same kek as when the dump was taken
```

Not covered yet: access from other devices (phone, LAN). That needs TLS and a reverse proxy; Telegram covers
mobile meanwhile.
