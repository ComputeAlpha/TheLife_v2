#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <memory>
#include <string>

namespace thelife::db {

struct Readiness {
  bool ok = false;
  std::string detail;  // short reason, safe to expose (no credentials)
};

// Process-wide connection pool. Stage 0 only exposes readiness; TenantTx arrives in Stage 1
// (backend_lld.md §3), and all queries will go through it.
class Database {
 public:
  // Empty conninfo → no database configured (service still starts; /readyz reports not ready).
  // queryTimeoutSec bounds every query, including ones queued while no connection is available,
  // so an unreachable database fails fast instead of hanging requests.
  static void init(const std::string& conninfo, std::size_t connections, double queryTimeoutSec);
  static Database& instance();

  bool configured() const { return client_ != nullptr; }
  drogon::Task<Readiness> checkReady(const std::string& expectedSchemaVersion) const;

 private:
  drogon::orm::DbClientPtr client_;
};

}  // namespace thelife::db
