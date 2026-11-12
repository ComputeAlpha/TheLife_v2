#include "db/database.h"

#include <drogon/orm/Exception.h>

namespace thelife::db {

namespace {
Database g_instance;
}

void Database::init(const std::string& conninfo, std::size_t connections, double queryTimeoutSec) {
  if (conninfo.empty()) {
    g_instance.client_.reset();
    return;
  }
  g_instance.client_ = drogon::orm::DbClient::newPgClient(conninfo, connections);
  g_instance.client_->setTimeout(queryTimeoutSec);
}

Database& Database::instance() { return g_instance; }

drogon::Task<Readiness> Database::checkReady(const std::string& expectedSchemaVersion) const {
  if (!client_) co_return Readiness{false, "database not configured"};
  try {
    // dbmate bookkeeping table; versions are zero-padded so max() is the latest.
    auto rows = co_await client_->execSqlCoro("SELECT coalesce(max(version), '') AS v FROM schema_migrations");
    const auto current = rows[0]["v"].as<std::string>();
    if (current != expectedSchemaVersion) {
      co_return Readiness{false, "schema version " + (current.empty() ? std::string("none") : current) +
                                     ", expected " + expectedSchemaVersion};
    }
    co_return Readiness{true, "ok"};
  } catch (const drogon::orm::DrogonDbException&) {
    // Do not echo driver messages: they can contain host or role names.
    co_return Readiness{false, "database unreachable or not migrated"};
  }
}

}  // namespace thelife::db
