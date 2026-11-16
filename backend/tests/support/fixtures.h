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

// For a test server running proactive work: seeds the tenant's default routines (morning, evening, weekly) and
// switches them off. Call before the server starts claiming jobs. Without this, other tests' cross-tenant
// scheduling seeds them enabled at any moment, and inside a routine's window (06:00–10:00 UTC for the morning one)
// a routine turn takes the test's scripted model replies.
void disableDefaultRoutines(const std::string& tenantId);

drogon::orm::DbClientPtr appDb();
drogon::orm::DbClientPtr adminDb();

}  // namespace thelife::test

#define THELIFE_SKIP_WITHOUT_DB()                                                          \
  do {                                                                                     \
    if (!::thelife::test::dbAvailable()) GTEST_SKIP() << "THELIFE_TEST_DB_URL / THELIFE_TEST_ADMIN_DB_URL not set"; \
  } while (0)
