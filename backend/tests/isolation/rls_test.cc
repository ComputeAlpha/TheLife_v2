// Isolation suite (backend_lld.md §4.4): catalog guarantees, cross-tenant leak checks and role privileges,
// executed directly against Postgres as the real roles.
#include <drogon/orm/Exception.h>
#include <gtest/gtest.h>

#include "core/crypto.h"
#include "core/ids.h"
#include "support/fixtures.h"

using namespace thelife::test;
using drogon::orm::DbClientPtr;

namespace {

// Runs statements in one transaction with an optional tenant context, then rolls back.
template <typename Fn>
void withTenant(const std::string& tenantId, Fn fn) {
  auto tx = appDb()->newTransaction();
  if (!tenantId.empty()) tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenantId);
  fn(*tx);
  tx->rollback();
}

std::vector<std::string> tenantTables() {
  // Every public table and view (views must be security_invoker so the tables' policies apply).
  const auto r = appDb()->execSqlSync(
      "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
      "WHERE n.nspname = 'public' AND c.relkind IN ('r', 'v') AND c.relname <> 'schema_migrations' ORDER BY 1");
  std::vector<std::string> out;
  for (const auto& row : r) out.push_back(row[0].as<std::string>());
  return out;
}

class Isolation : public ::testing::Test {
 protected:
  void SetUp() override { THELIFE_SKIP_WITHOUT_DB(); }
};

TEST_F(Isolation, EveryTableHasForcedRlsAndTenantPolicy) {
  const auto r = appDb()->execSqlSync(
      "SELECT c.relname, c.relrowsecurity, c.relforcerowsecurity, "
      "  EXISTS (SELECT 1 FROM pg_policy p WHERE p.polrelid = c.oid AND p.polname = 'tenant_isolation') AS pol, "
      "  EXISTS (SELECT 1 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attname = 'tenant_id' "
      "          AND NOT a.attisdropped) AS has_tenant "
      "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
      "WHERE n.nspname = 'public' AND c.relkind IN ('r', 'p') AND c.relname <> 'schema_migrations'");
  ASSERT_GT(r.size(), 0u);
  for (const auto& row : r) {
    const auto name = row["relname"].as<std::string>();
    EXPECT_TRUE(row["relrowsecurity"].as<bool>()) << name << ": RLS not enabled";
    EXPECT_TRUE(row["relforcerowsecurity"].as<bool>()) << name << ": RLS not forced";
    EXPECT_TRUE(row["pol"].as<bool>()) << name << ": no tenant_isolation policy";
    if (name != "tenants") {
      EXPECT_TRUE(row["has_tenant"].as<bool>()) << name << ": no tenant_id column";
    }
  }
}

TEST_F(Isolation, NoTenantContextSeesNothing) {
  createTenantWithUser();  // make sure rows exist
  for (const auto& table : tenantTables()) {
    withTenant("", [&](auto& tx) {
      const auto r = tx.execSqlSync("SELECT count(*) AS n FROM " + table);
      EXPECT_EQ(r[0]["n"].template as<int64_t>(), 0) << table << " visible without tenant context";
    });
  }
}

TEST_F(Isolation, TenantSeesOnlyItsOwnRowsInEveryTable) {
  auto a = createTenantWithUser();
  auto b = createTenantWithUser();
  for (const auto& table : tenantTables()) {
    const std::string col = table == "tenants" ? "id" : "tenant_id";
    withTenant(a.tenantId, [&](auto& tx) {
      const auto r = tx.execSqlSync("SELECT count(*) AS n FROM " + table + " WHERE " + col + " <> $1", a.tenantId);
      EXPECT_EQ(r[0]["n"].template as<int64_t>(), 0) << table << " leaks other tenants' rows";
    });
  }
  // and b is really there, just hidden from a
  withTenant(b.tenantId, [&](auto& tx) {
    const auto r = tx.execSqlSync("SELECT count(*) AS n FROM users WHERE id = $1", b.userId);
    EXPECT_EQ(r[0]["n"].template as<int64_t>(), 1);
  });
}

TEST_F(Isolation, CannotWriteIntoAnotherTenant) {
  auto a = createTenantWithUser();
  auto b = createTenantWithUser();
  const auto tok = thelife::crypto::newToken(thelife::crypto::TokenKind::UserSession);
  withTenant(a.tenantId, [&](auto& tx) {
    EXPECT_THROW(tx.execSqlSync("INSERT INTO credentials (id, tenant_id, kind, user_id, token_prefix, token_hash) "
                                "VALUES ($1, $2, 'user_session', $3, $4, $5::bytea)",
                                thelife::ids::newUuidV7(), b.tenantId, b.userId, tok.prefix,
                                thelife::crypto::pgByteaHex(tok.hash)),
                 drogon::orm::DrogonDbException);
  });
  // Updates of other tenants' rows silently match nothing.
  withTenant(a.tenantId, [&](auto& tx) {
    const auto r = tx.execSqlSync("UPDATE users SET display_name = 'pwned' WHERE id = $1", b.userId);
    EXPECT_EQ(r.affectedRows(), 0u);
  });
}

TEST_F(Isolation, AppRoleCannotEscalate) {
  auto a = createTenantWithUser();
  auto expectDenied = [&](const std::string& sql) {
    withTenant(a.tenantId, [&](auto& tx) {
      EXPECT_THROW(tx.execSqlSync(sql), drogon::orm::DrogonDbException) << sql;
    });
  };
  expectDenied("SELECT admin.create_tenant(gen_random_uuid(), 'x')");
  expectDenied("SELECT * FROM admin.list_tenants()");
  expectDenied("INSERT INTO tenants (id, name) VALUES (gen_random_uuid(), 'x')");
  expectDenied("UPDATE tenants SET agent_writes_enabled = true");
  expectDenied("UPDATE users SET role = 'tenant_admin'");
  expectDenied("UPDATE users SET status = 'active'");
  expectDenied("UPDATE credentials SET scopes = '{}'");
  expectDenied("DELETE FROM credentials");
  expectDenied("CREATE TABLE x (i int)");
  expectDenied("SET ROLE thelife_definer");
}

TEST_F(Isolation, AdminRoleOnlyReachesAdminFunctions) {
  for (const std::string sql : {"SELECT count(*) FROM users", "SELECT count(*) FROM credentials",
                                "SELECT * FROM auth.find_login('x@y.z')"}) {
    EXPECT_THROW(adminDb()->execSqlSync(sql), drogon::orm::DrogonDbException) << sql;
  }
  EXPECT_NO_THROW(adminDb()->execSqlSync("SELECT * FROM admin.list_tenants()"));
}

TEST_F(Isolation, ImmutableResourceColumnsAreGuarded) {
  // devices carries the common resource columns; created_by/origin must never change.
  auto a = createTenantWithUser();
  withTenant(a.tenantId, [&](auto& tx) {
    const auto id = thelife::ids::newUuidV7();
    tx.execSqlSync("INSERT INTO devices (id, tenant_id, created_by_kind, created_by_id, user_id, name, platform) "
                   "VALUES ($1, $2, 'user', $3, $3, 'laptop', 'linux-x11')",
                   id, a.tenantId, a.userId);
    const auto v = tx.execSqlSync("UPDATE devices SET name = 'desk' WHERE id = $1 RETURNING version", id);
    EXPECT_EQ(v[0]["version"].template as<int>(), 2);
    EXPECT_THROW(tx.execSqlSync("UPDATE devices SET origin = 'inferred' WHERE id = $1", id),
                 drogon::orm::DrogonDbException);
  });
}

TEST_F(Isolation, DetectionsAreAppendOnlyForTheAppRole) {
  auto a = createTenantWithUser();
  const auto insert = [&](auto& tx, const std::string& extra) {
    const auto id = thelife::ids::newUuidV7();
    tx.execSqlSync("INSERT INTO detections (id, tenant_id, created_by_kind, source, started_at, ended_at, area, "
                   "category, summary" + std::string(extra.empty() ? "" : ", supersedes_id") + ") "
                   "VALUES ($1, $2, 'user', 'external', now(), now(), 'work', 'c', 's'" +
                   (extra.empty() ? "" : ", '" + extra + "'") + ")",
                   id, a.tenantId);
    return id;
  };
  auto expectDenied = [&](const std::string& sqlTemplate) {
    withTenant(a.tenantId, [&](auto& tx) {
      const auto id = insert(tx, "");
      std::string sql = sqlTemplate;
      for (auto p = sql.find("$ID"); p != std::string::npos; p = sql.find("$ID")) sql.replace(p, 3, id);
      EXPECT_THROW(tx.execSqlSync(sql), drogon::orm::DrogonDbException) << sql;
    });
  };
  expectDenied("UPDATE detections SET summary = 'rewritten' WHERE id = '$ID'");
  expectDenied("UPDATE detections SET status = 'confirmed' WHERE id = '$ID'");
  expectDenied("DELETE FROM detections WHERE id = '$ID'");
  expectDenied("INSERT INTO detection_events (id, tenant_id, detection_id, actor_kind, from_status, to_status) "
               "SELECT gen_random_uuid(), tenant_id, id, 'user', 'detected', 'confirmed' FROM detections "
               "WHERE id = '$ID'");
  // supersedes_id is reserved for claims
  withTenant(a.tenantId, [&](auto& tx) {
    const auto first = insert(tx, "");
    EXPECT_THROW(insert(tx, first), drogon::orm::DrogonDbException);
  });
  // the sanctioned path changes status and always leaves an event
  withTenant(a.tenantId, [&](auto& tx) {
    tx.execSqlSync("SELECT set_config('app.principal_kind', 'user', true)");
    const auto id = insert(tx, "");
    const auto r = tx.execSqlSync("SELECT (app.detection_transition($1, 'confirm', NULL)).status", id);
    EXPECT_EQ(r[0][0].template as<std::string>(), "confirmed");
    const auto ev = tx.execSqlSync("SELECT count(*) FROM detection_events WHERE detection_id = $1", id);
    EXPECT_EQ(ev[0][0].template as<int64_t>(), 1);
  });
  // and cannot reach another tenant's detection
  auto b = createTenantWithUser();
  const auto foreign = thelife::ids::newUuidV7();
  {
    auto tx = appDb()->newTransaction();  // committed when released
    tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", b.tenantId);
    tx->execSqlSync("INSERT INTO detections (id, tenant_id, created_by_kind, source, started_at, ended_at, area, "
                    "category, summary) VALUES ($1, $2, 'user', 'external', now(), now(), 'work', 'c', 's')",
                    foreign, b.tenantId);
  }
  withTenant(a.tenantId, [&](auto& tx) {
    EXPECT_THROW(tx.execSqlSync("SELECT app.detection_transition($1, 'confirm', NULL)", foreign),
                 drogon::orm::DrogonDbException);
  });
}

}  // namespace

// Every contract-bound resource maps 1:1 onto its table: each writable and output field is a column
// (except derived ones such as created_by), and the table carries the common resource columns.
#include "crud/resource.h"
#include "modules/modules.h"
#include "tools/manifest.h"
#include "tools/registry.h"

namespace {
TEST_F(Isolation, ContractFieldsMatchTableColumns) {
  static thelife::tools::ToolRegistry reg(thelife::tools::embeddedManifest());
  if (thelife::crud::boundResources().empty()) thelife::modules::bindAll(reg);
  ASSERT_FALSE(thelife::crud::boundResources().empty());
  for (const auto& [prefix, table] : thelife::crud::boundResources()) {
    const auto cols = appDb()->execSqlSync(
        "SELECT a.attname FROM pg_attribute a WHERE a.attrelid = $1::regclass AND a.attnum > 0 AND NOT a.attisdropped",
        table);
    std::set<std::string> have;
    for (const auto& r : cols) have.insert(r[0].as<std::string>());
    const auto& out = reg.tool(prefix + ".get").outputSchema["properties"];
    for (const auto& field : out.getMemberNames()) {
      if (field == "created_by") {
        EXPECT_TRUE(have.count("created_by_kind") && have.count("created_by_id")) << table;
        continue;
      }
      EXPECT_TRUE(have.count(field)) << table << " lacks column for contract field " << field;
    }
  }
}
}  // namespace

