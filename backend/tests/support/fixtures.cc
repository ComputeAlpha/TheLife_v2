#include "support/fixtures.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "core/crypto.h"
#include "core/ids.h"

namespace thelife::test {

namespace {
std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}
}  // namespace

std::string appDbUrl() { return env("THELIFE_TEST_DB_URL"); }
std::string adminDbUrl() { return env("THELIFE_TEST_ADMIN_DB_URL"); }
bool dbAvailable() { return !appDbUrl().empty() && !adminDbUrl().empty(); }

drogon::orm::DbClientPtr appDb() {
  static auto c = [] {
    auto db = drogon::orm::DbClient::newPgClient(appDbUrl(), 2);
    db->setTimeout(10.0);
    return db;
  }();
  return c;
}

drogon::orm::DbClientPtr adminDb() {
  static auto c = [] {
    auto db = drogon::orm::DbClient::newPgClient(adminDbUrl(), 1);
    db->setTimeout(10.0);
    return db;
  }();
  return c;
}

TestUser createUser(const std::string& tenantId) {
  // Hashing is slow by design; cache one hash for the fixed test password.
  static const std::string kPassword = "correct horse battery staple";
  static const std::string kHash = crypto::hashPassword(kPassword);
  TestUser u;
  u.tenantId = tenantId;
  u.email = "user-" + ids::newUuidV7() + "@test.thelife.local";
  u.password = kPassword;
  const auto r = adminDb()->execSqlSync("SELECT admin.create_user($1, $2, $3, $4, $5, 'user', 'UTC') AS id",
                                        ids::newUuidV7(), tenantId, u.email, std::string("Test User"), kHash);
  u.userId = r[0]["id"].as<std::string>();
  return u;
}

void disableDefaultRoutines(const std::string& tenantId) {
  // Another test's seeding of this tenant may commit concurrently (the unique index lets one of them win), so
  // repeat until the routines exist and are all off.
  for (int i = 0; i < 100; ++i) {
    std::promise<bool> committed;
    int total = 0, on = 0;
    {
      auto tx = appDb()->newTransaction();
      tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenantId);
      tx->execSqlSync(
          "DO $$ BEGIN PERFORM ops.enqueue_proactive_ticks(); UPDATE routines SET enabled = false; END $$");
      const auto r = tx->execSqlSync("SELECT count(*)::int AS n, (count(*) FILTER (WHERE enabled))::int AS on "
                                     "FROM routines");
      total = r[0]["n"].as<int>();
      on = r[0]["on"].as<int>();
      tx->setCommitCallback([&committed](bool ok) { committed.set_value(ok); });
    }
    if (committed.get_future().get() && total > 0 && on == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("could not switch off the default routines of tenant " + tenantId);
}

TestUser createTenantWithUser() {
  const auto t = adminDb()->execSqlSync("SELECT admin.create_tenant($1, $2) AS id", ids::newUuidV7(),
                                        std::string("test tenant"));
  return createUser(t[0]["id"].as<std::string>());
}

}  // namespace thelife::test
