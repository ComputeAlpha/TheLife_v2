#include "db/job_store.h"

#include "core/ids.h"
#include "core/json.h"
#include "db/database.h"

namespace thelife::db::jobs {

namespace {
const char* kInsert =
    "INSERT INTO ops.jobs (id, tenant_id, kind, payload, dedupe_key, run_at, priority, max_attempts) "
    "VALUES ($1::uuid, $2::uuid, $3, $4::jsonb, $5, now() + make_interval(secs => $6::int), $7::smallint, $8::int) "
    "ON CONFLICT (kind, dedupe_key) WHERE dedupe_key IS NOT NULL AND status IN ('queued', 'running') DO NOTHING";

std::vector<std::optional<std::string>> params(const std::optional<std::string>& tenant, const NewJob& j) {
  return {ids::newUuidV7(), tenant, j.kind, json::compact(j.payload), j.dedupeKey, std::to_string(j.delaySec),
          std::to_string(j.priority), std::to_string(j.maxAttempts)};
}

drogon::orm::DbClientPtr client() {
  const auto& c = Database::instance().client();
  if (!c) throw std::runtime_error("database not configured");
  return c;
}
}  // namespace

drogon::Task<void> enqueue(TenantTx& tx, NewJob job) {
  co_await tx.execParams(kInsert, params(tx.tenantId(), job));
}

drogon::Task<void> enqueueSystem(std::optional<std::string> tenantId, NewJob job) {
  const auto p = params(tenantId, job);
  co_await client()->execSqlCoro(kInsert, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

drogon::Task<std::vector<ClaimedJob>> claim(std::string worker, std::vector<std::string> kinds, int n,
                                            std::optional<std::string> onlyTenant) {
  std::string arr = "{";
  for (std::size_t i = 0; i < kinds.size(); ++i) arr += (i ? "," : "") + kinds[i];
  arr += "}";
  const auto r = co_await client()->execSqlCoro(
      "UPDATE ops.jobs j SET status = 'running', locked_by = $1, locked_at = now(), attempts = j.attempts + 1, "
      "updated_at = now() "
      "WHERE j.id IN (SELECT id FROM ops.jobs WHERE status = 'queued' AND run_at <= now() AND kind = ANY($2::text[]) "
      "                 AND (nullif($4, '') IS NULL OR tenant_id = nullif($4, '')::uuid) "
      "               ORDER BY priority, run_at FOR UPDATE SKIP LOCKED LIMIT $3::int) "
      "RETURNING j.id::text AS id, coalesce(j.tenant_id::text, '') AS tenant, j.kind, j.payload::text AS payload, "
      "  j.attempts, j.max_attempts",
      worker, arr, std::to_string(n), onlyTenant.value_or(""));
  std::vector<ClaimedJob> out;
  for (const auto& row : r) {
    ClaimedJob j;
    j.id = row["id"].as<std::string>();
    j.tenantId = row["tenant"].as<std::string>();
    j.kind = row["kind"].as<std::string>();
    j.payload = json::parse(row["payload"].as<std::string>());
    j.attempts = row["attempts"].as<int>();
    j.maxAttempts = row["max_attempts"].as<int>();
    out.push_back(std::move(j));
  }
  co_return out;
}

drogon::Task<void> complete(std::string id) {
  co_await client()->execSqlCoro(
      "UPDATE ops.jobs SET status = 'done', finished_at = now(), updated_at = now(), last_error = NULL "
      "WHERE id = $1::uuid",
      id);
}

drogon::Task<void> fail(std::string id, std::string error, int attempts, int maxAttempts) {
  // Exponential backoff: 1, 2, 4, 8 ... minutes, capped at 6 h.
  const int delay = std::min(6 * 3600, 60 * (1 << std::min(attempts - 1, 10)));
  co_await client()->execSqlCoro(
      "UPDATE ops.jobs SET status = CASE WHEN $3::int >= max_attempts THEN 'dead' ELSE 'queued' END, "
      "run_at = now() + make_interval(secs => $4::int), last_error = left($2, 1000), locked_by = NULL, "
      "updated_at = now(), finished_at = CASE WHEN $3::int >= max_attempts THEN now() END WHERE id = $1::uuid",
      id, error, std::to_string(attempts), std::to_string(delay));
  (void)maxAttempts;
}

drogon::Task<void> defer(std::string id, std::chrono::system_clock::time_point runAt, std::string reason) {
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(runAt.time_since_epoch()).count();
  co_await client()->execSqlCoro(
      "UPDATE ops.jobs SET status = 'queued', run_at = to_timestamp($2::bigint), attempts = greatest(attempts - 1, 0), "
      "last_error = left($3, 1000), locked_by = NULL, updated_at = now() WHERE id = $1::uuid",
      id, std::to_string(secs), reason);
}

drogon::Task<Housekeeping> housekeeping() {
  Housekeeping h;
  auto c = client();
  h.dayCloses = (co_await c->execSqlCoro("SELECT ops.enqueue_day_closes() AS n"))[0]["n"].as<int>();
  h.staleTurns = (co_await c->execSqlCoro("SELECT ops.fail_stale_turns() AS n"))[0]["n"].as<int>();
  h.idempotencyKeys = (co_await c->execSqlCoro("SELECT ops.gc_idempotency_keys() AS n"))[0]["n"].as<int>();
  h.ticks = (co_await c->execSqlCoro("SELECT ops.enqueue_proactive_ticks() AS n"))[0]["n"].as<int>();
  // A worker that died mid-job leaves it running; after 15 minutes it is retried (counted as an attempt).
  const auto r = co_await c->execSqlCoro(
      "UPDATE ops.jobs SET status = 'queued', locked_by = NULL, last_error = 'worker lost', updated_at = now() "
      "WHERE status = 'running' AND locked_at < now() - interval '15 minutes' RETURNING 1");
  h.requeued = static_cast<int>(r.size());
  co_return h;
}

}  // namespace thelife::db::jobs
