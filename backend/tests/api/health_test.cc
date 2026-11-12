// Stage 0 exit gate: the empty service builds, serves /healthz, and /readyz reflects the database.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "core/build_info.h"
#include "support/http.h"
#include "support/server_process.h"

using thelife::test::httpGet;
using thelife::test::ServerProcess;

namespace {
std::string testDbUrl() {
  const char* v = std::getenv("THELIFE_TEST_DB_URL");
  return v ? v : "";
}
}  // namespace

TEST(Health, HealthzReportsOkAndVersion) {
  ServerProcess server;
  const auto r = httpGet(server.baseUrl() + "/healthz");
  ASSERT_EQ(r.status, 200);
  EXPECT_NE(r.contentType.find("application/json"), std::string::npos);
  EXPECT_EQ(r.json["status"].asString(), "ok");
  EXPECT_EQ(r.json["version"].asString(), thelife::build::kVersion);
}

TEST(Health, UnknownPathIs404) {
  ServerProcess server;
  const auto r = httpGet(server.baseUrl() + "/healthz-does-not-exist");
  EXPECT_EQ(r.status, 404);
}

TEST(Health, ReadyzWithoutDatabaseIs503) {
  ServerProcess server;  // no THELIFE_DB_URL
  const auto r = httpGet(server.baseUrl() + "/readyz");
  ASSERT_EQ(r.status, 503);
  EXPECT_EQ(r.json["status"].asString(), "not_ready");
  EXPECT_EQ(r.json["checks"]["database"].asString(), "database not configured");
}

TEST(Health, ReadyzWithUnreachableDatabaseIs503WithoutLeakingDetails) {
  ServerProcess server({{"THELIFE_DB_URL", "postgresql://nobody:secretpw@127.0.0.1:1/nowhere"},
                        {"THELIFE_DB_QUERY_TIMEOUT_S", "1"}});
  // Must answer within the query timeout instead of hanging on a queue with no connections.
  const auto r = httpGet(server.baseUrl() + "/readyz", 5.0);
  ASSERT_EQ(r.status, 503);
  EXPECT_EQ(r.body.find("secretpw"), std::string::npos);
  EXPECT_EQ(r.body.find("nobody"), std::string::npos);
}

TEST(Health, ReadyzWithMigratedDatabaseIs200) {
  const auto url = testDbUrl();
  if (url.empty()) GTEST_SKIP() << "THELIFE_TEST_DB_URL not set";
  ServerProcess server({{"THELIFE_DB_URL", url}});
  // The pool connects asynchronously; allow a short warm-up.
  thelife::test::HttpResult r;
  for (int i = 0; i < 50; ++i) {
    r = httpGet(server.baseUrl() + "/readyz");
    if (r.status == 200) break;
    usleep(100000);
  }
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["status"].asString(), "ready");
  EXPECT_EQ(r.json["schema_version"].asString(), thelife::build::kSchemaVersion);
}
