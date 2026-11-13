#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

#include "db/job_store.h"

// In-process job worker (backend_lld.md §10): claims due jobs, runs registered handlers, retries with backoff.
namespace thelife::jobs {

using Job = db::jobs::ClaimedJob;
using Handler = std::function<drogon::Task<void>(const Job&)>;

// Thrown by a handler to run the job again at `runAt` without counting a failed attempt (assistant offline until
// the quota resets, rate limited).
struct Defer : std::runtime_error {
  Defer(std::chrono::system_clock::time_point at, const std::string& why) : std::runtime_error(why), runAt(at) {}
  std::chrono::system_clock::time_point runAt;
};

void registerHandler(const std::string& kind, Handler h);

// Polls every THELIFE_JOBS_POLL_MS (default 2000) with up to THELIFE_JOBS_CONCURRENCY (default 2) jobs in flight,
// and runs housekeeping (day close scheduling, stale turns, idempotency GC) every minute. THELIFE_JOBS=off
// disables it; THELIFE_JOBS_HOUSEKEEPING=off keeps the worker but skips scheduling; THELIFE_JOBS_TENANT=<uuid>
// claims only that tenant's jobs (diagnostics, tests on a shared database).
void startWorker();

// Claims and runs up to `max` due jobs now; returns how many ran.
drogon::Task<int> runDue(int max);

}  // namespace thelife::jobs
