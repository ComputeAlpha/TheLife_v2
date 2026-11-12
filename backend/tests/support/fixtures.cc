#include "support/fixtures.h"

#include <cstdlib>
#include <mutex>

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

TestUser createTenantWithUser() {
  const auto t = adminDb()->execSqlSync("SELECT admin.create_tenant($1, $2) AS id", ids::newUuidV7(),
                                        std::string("test tenant"));
  return createUser(t[0]["id"].as<std::string>());
}

}  // namespace thelife::test
