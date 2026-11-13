#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "db/tenant_tx.h"

// ops.jobs access (backend_lld.md §10). The queue is deployment state, claimed across tenants; handlers open a
// TenantTx for the job's tenant.
namespace thelife::db::jobs {

struct ClaimedJob {
  std::string id, tenantId, kind;
  Json::Value payload;
  int attempts = 0, maxAttempts = 5;
};

struct NewJob {
  std::string kind;
  Json::Value payload = Json::Value(Json::objectValue);
  std::optional<std::string> dedupeKey;  // coalesces with a queued/running job of the same kind and key
  int delaySec = 0;
  int priority = 3;
  int maxAttempts = 5;
};

// Inside a tenant transaction, so the job exists only if the change that caused it commits.
drogon::Task<void> enqueue(TenantTx& tx, NewJob job);
// Deployment-wide or from code without a transaction.
drogon::Task<void> enqueueSystem(std::optional<std::string> tenantId, NewJob job);

// onlyTenant limits the claim to one tenant's jobs (THELIFE_JOBS_TENANT: diagnostics and tests).
drogon::Task<std::vector<ClaimedJob>> claim(std::string worker, std::vector<std::string> kinds, int n,
                                            std::optional<std::string> onlyTenant = std::nullopt);
drogon::Task<void> complete(std::string id);
// Failure: retried after backoff, or dead once attempts reach max_attempts.
drogon::Task<void> fail(std::string id, std::string error, int attempts, int maxAttempts);
// Not a failure (model offline, rate limited): back to the queue for runAt, attempt not counted.
drogon::Task<void> defer(std::string id, std::chrono::system_clock::time_point runAt, std::string reason);

// Scheduler housekeeping (SECURITY DEFINER functions plus requeue of jobs stuck in running).
struct Housekeeping {
  int dayCloses = 0, staleTurns = 0, idempotencyKeys = 0, requeued = 0;
};
drogon::Task<Housekeeping> housekeeping();

}  // namespace thelife::db::jobs
