#include "jobs/worker.h"

#include <drogon/drogon.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>

#include "core/env.h"
#include "core/errors.h"

namespace thelife::jobs {

namespace {

std::mutex handlersMu;
std::map<std::string, Handler>& handlers() {
  static std::map<std::string, Handler> h;
  return h;
}
std::atomic<int> inFlight{0};
int concurrency = 2;
std::optional<std::string> onlyTenant;

std::string workerName() {
  char host[64] = {0};
  gethostname(host, sizeof host - 1);
  return std::string(host) + ":" + std::to_string(getpid());
}

drogon::Task<void> runOne(Job job) {
  Handler h;
  {
    std::lock_guard lock(handlersMu);
    const auto it = handlers().find(job.kind);
    if (it != handlers().end()) h = it->second;
  }
  std::string error;
  std::optional<Defer> deferred;
  if (!h) {
    error = "no handler for " + job.kind;
  } else {
    try {
      co_await h(job);
    } catch (const Defer& d) {
      deferred = d;
    } catch (const ApiError& e) {
      error = e.title() + (e.detail().empty() ? "" : ": " + e.detail());
    } catch (const std::exception& e) {
      error = e.what();
    }
  }
  try {
    if (deferred) {
      LOG_INFO << "job " << job.kind << " " << job.id << " deferred: " << deferred->what();
      co_await db::jobs::defer(job.id, deferred->runAt, deferred->what());
    } else if (!error.empty()) {
      LOG_WARN << "job " << job.kind << " " << job.id << " attempt " << job.attempts << " failed: " << error;
      co_await db::jobs::fail(job.id, error, job.attempts, job.maxAttempts);
    } else {
      co_await db::jobs::complete(job.id);
    }
  } catch (const std::exception& e) {
    LOG_ERROR << "job " << job.id << " state not saved: " << e.what();
  }
}

std::vector<std::string> kinds() {
  std::lock_guard lock(handlersMu);
  std::vector<std::string> out;
  for (const auto& [k, _] : handlers()) out.push_back(k);
  return out;
}

void poll() {
  const int free = concurrency - inFlight.load();
  if (free <= 0) return;
  inFlight += free;  // reserve slots until the claim returns
  drogon::async_run([free]() -> drogon::Task<void> {
    std::vector<Job> claimed;
    try {
      claimed = co_await db::jobs::claim(workerName(), kinds(), free, onlyTenant);
    } catch (const std::exception& e) {
      LOG_ERROR << "job claim failed: " << e.what();
    }
    inFlight -= free - static_cast<int>(claimed.size());
    for (auto& j : claimed) {
      drogon::async_run([j = std::move(j)]() -> drogon::Task<void> {
        co_await runOne(j);
        --inFlight;
      });
    }
  });
}

}  // namespace

void registerHandler(const std::string& kind, Handler h) {
  std::lock_guard lock(handlersMu);
  handlers()[kind] = std::move(h);
}

drogon::Task<int> runDue(int max) {
  auto claimed = co_await db::jobs::claim(workerName(), kinds(), max, onlyTenant);
  for (auto& j : claimed) co_await runOne(j);
  co_return static_cast<int>(claimed.size());
}

void startWorker() {
  if (env::get("THELIFE_JOBS", "on") == "off") {
    LOG_INFO << "jobs: worker disabled (THELIFE_JOBS=off)";
    return;
  }
  concurrency = env::getInt("THELIFE_JOBS_CONCURRENCY", 2, 1, 16);
  if (const auto t = env::get("THELIFE_JOBS_TENANT"); !t.empty()) onlyTenant = t;
  const double pollSec = env::getInt("THELIFE_JOBS_POLL_MS", 2000, 50, 60000) / 1000.0;
  const double houseSec = env::getInt("THELIFE_JOBS_HOUSEKEEPING_S", 60, 1, 3600);
  auto* loop = drogon::app().getLoop();
  loop->runEvery(pollSec, [] { poll(); });
  if (env::get("THELIFE_JOBS_HOUSEKEEPING", "on") == "off") {
    LOG_INFO << "jobs: worker started without housekeeping";
    return;
  }
  loop->runEvery(houseSec, [] {
    drogon::async_run([]() -> drogon::Task<void> {
      try {
        const auto h = co_await db::jobs::housekeeping();
        if (h.dayCloses || h.staleTurns || h.requeued) {
          LOG_INFO << "jobs: queued " << h.dayCloses << " day closes, failed " << h.staleTurns
                   << " stale turns, requeued " << h.requeued << " lost jobs";
        }
      } catch (const std::exception& e) {
        LOG_ERROR << "jobs housekeeping failed: " << e.what();
      }
    });
  });
  // First housekeeping soon after start, so a restart catches up on missed day closes.
  loop->runAfter(1.0, [] {
    drogon::async_run([]() -> drogon::Task<void> {
      try {
        co_await db::jobs::housekeeping();
      } catch (const std::exception& e) {
        LOG_ERROR << "jobs housekeeping failed: " << e.what();
      }
    });
  });
  LOG_INFO << "jobs: worker started (" << concurrency << " slots, poll " << pollSec << " s)";
}

}  // namespace thelife::jobs
