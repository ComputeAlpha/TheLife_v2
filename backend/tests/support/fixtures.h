#pragma once

#include <drogon/orm/DbClient.h>

#include <string>

namespace thelife::test {

// Connection URLs for integration tests. Tests that need the database call skipIfNoDb() first.
std::string appDbUrl();    // THELIFE_TEST_DB_URL (thelife_app)
std::string adminDbUrl();  // THELIFE_TEST_ADMIN_DB_URL (thelife_admin)
bool dbAvailable();

struct TestUser {
  std::string tenantId;
  std::string userId;
  std::string email;
  std::string password;
};

// Creates a fresh tenant with one user through the admin.* functions, exactly like thelife-admin does.
// Every test gets its own tenant, so tests never interfere and nothing needs truncating.
TestUser createTenantWithUser();
// Adds a user to an existing tenant.
TestUser createUser(const std::string& tenantId);

drogon::orm::DbClientPtr appDb();
drogon::orm::DbClientPtr adminDb();

}  // namespace thelife::test

#define THELIFE_SKIP_WITHOUT_DB()                                                          \
  do {                                                                                     \
    if (!::thelife::test::dbAvailable()) GTEST_SKIP() << "THELIFE_TEST_DB_URL / THELIFE_TEST_ADMIN_DB_URL not set"; \
  } while (0)
