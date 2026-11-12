// Access rules derived from the tool manifest (docs/tool_registry.md §5), applied to every Stage 1 REST tool:
//   * a device token reaches none of them (collector_lld.md §8: only ingest/config/heartbeat/notifications)
//   * an API key without the tool's scope gets 403; user-only tools refuse API keys
//   * updates and deletes without If-Match get 428
//   * read tools aimed at an unknown or foreign id get 404
//   * a method the path does not serve gets 405 with an Allow header (RFC 9110)
#include <gtest/gtest.h>

#include <map>
#include <regex>
#include <set>

#include "core/ids.h"
#include "core/json.h"
#include "support/api_client.h"
#include "tools/manifest.h"

using namespace thelife::test;
using thelife::json::parse;

namespace {

std::string fill(const std::string& path) {
  // /api/v1/tasks/{id}/occurrences/{date}:complete → concrete URL below /api/v1
  std::string out = std::regex_replace(path, std::regex("\\{date\\}"), "2026-01-05");
  out = std::regex_replace(out, std::regex("\\{[a-z_]+\\}"), thelife::ids::newUuidV7());
  return out.substr(std::string("/api/v1").size());
}

std::vector<const thelife::tools::ToolDef*> stage1RestTools() {
  std::vector<const thelife::tools::ToolDef*> out;
  for (const auto& [name, def] : thelife::tools::embeddedManifest().tools) {
    if (def.stage <= 1 && def.rest && def.rest->path.rfind("/api/v1/", 0) == 0) out.push_back(&def);
  }
  return out;
}

Json::Value bodyFor(const thelife::tools::ToolDef& def) {
  return def.rest->method == "GET" || def.rest->method == "DELETE" ? Json::Value() : Json::Value(Json::objectValue);
}

class ContractMatrix : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (!dbAvailable()) return;
    server_ = startServerWithDb().release();
    user_ = new TestUser(createTenantWithUser());
    const ApiClient owner(*server_, *user_);
    const auto key = owner.post("/api-keys", parse(R"({"name":"reader","scopes":["planning:read"]})"));
    const auto dev = owner.post("/devices", parse(R"({"name":"laptop","platform":"linux-x11"})"));
    if (key.status != 201 || dev.status != 201) throw std::runtime_error("setup failed: " + key.body + dev.body);
    keyToken_ = new std::string(key.json["token"].asString());
    deviceToken_ = new std::string(dev.json["token"].asString());
  }
  static void TearDownTestSuite() {
    delete server_;
    delete user_;
    delete keyToken_;
    delete deviceToken_;
  }
  void SetUp() override { THELIFE_SKIP_WITHOUT_DB(); }

  static ServerProcess* server_;
  static TestUser* user_;
  static std::string* keyToken_;
  static std::string* deviceToken_;
};
ServerProcess* ContractMatrix::server_ = nullptr;
TestUser* ContractMatrix::user_ = nullptr;
std::string* ContractMatrix::keyToken_ = nullptr;
std::string* ContractMatrix::deviceToken_ = nullptr;

TEST_F(ContractMatrix, DeviceTokenReachesNoStage1Tool) {
  const ApiClient device(*server_, *user_, *deviceToken_);
  for (const auto* def : stage1RestTools()) {
    ASSERT_FALSE(def->principals.count("device")) << def->name;
    const auto r = device.call(def->rest->method, fill(def->rest->path), bodyFor(*def), {{"If-Match", "\"1\""}});
    EXPECT_EQ(r.status, 403) << def->name << " " << r.body;
  }
}

TEST_F(ContractMatrix, ApiKeyNeedsTheToolsScope) {
  const ApiClient key(*server_, *user_, *keyToken_);
  int allowed = 0;
  for (const auto* def : stage1RestTools()) {
    const bool scoped = def->principals.count("api_key") &&
                        std::find(def->scopes.begin(), def->scopes.end(), "planning:read") != def->scopes.end();
    const auto r = key.call(def->rest->method, fill(def->rest->path), bodyFor(*def), {{"If-Match", "\"1\""}});
    if (scoped) {
      ++allowed;
      EXPECT_TRUE(r.status == 200 || r.status == 404) << def->name << " " << r.status << " " << r.body;
    } else {
      EXPECT_EQ(r.status, 403) << def->name << " " << r.body;
    }
  }
  EXPECT_EQ(allowed, 4);  // goals.list/get, plans.list/get
}

TEST_F(ContractMatrix, WritesRequireIfMatch) {
  const ApiClient user(*server_, *user_);
  int checked = 0;
  for (const auto* def : stage1RestTools()) {
    if (!def->rest->ifMatch) continue;
    ++checked;
    const auto r = user.call(def->rest->method, fill(def->rest->path), bodyFor(*def));
    EXPECT_EQ(r.status, 428) << def->name << " " << r.body;
  }
  EXPECT_GE(checked, 20);
}

TEST_F(ContractMatrix, TargetReadsAnswer404ForUnknownAndForeignIds) {
  const ApiClient user(*server_, *user_);
  const ApiClient other(*server_, createTenantWithUser());
  const auto goal = user.post("/goals", parse(R"({"title":"mine","area":"skill","horizon":"year"})"));
  ASSERT_EQ(goal.status, 201);
  for (const auto* def : stage1RestTools()) {
    if (!def->targetResource || def->rest->method != "GET") continue;
    const auto r = user.get(fill(def->rest->path));
    EXPECT_EQ(r.status, 404) << def->name << " " << r.body;
  }
  EXPECT_EQ(other.get("/goals/" + goal.json["id"].asString()).status, 404);
}

TEST_F(ContractMatrix, UnservedMethodsAnswer405WithAllow) {
  const ApiClient user(*server_, *user_);
  std::map<std::string, std::set<std::string>> methods;
  for (const auto* def : stage1RestTools()) methods[def->rest->path].insert(def->rest->method);
  for (const auto& [path, served] : methods) {
    for (const std::string m : {"GET", "POST", "PUT", "PATCH", "DELETE"}) {
      if (served.count(m)) continue;
      const auto r = user.call(m, fill(path), m == "GET" || m == "DELETE" ? Json::Value() : Json::Value(Json::objectValue));
      EXPECT_EQ(r.status, 405) << m << " " << path << " " << r.body;
      const auto allow = r.header("allow");
      for (const auto& s : served) EXPECT_NE(allow.find(s), std::string::npos) << m << " " << path << " " << allow;
      EXPECT_EQ(r.contentType.rfind("application/problem+json", 0), 0u) << path;
    }
  }
}

TEST_F(ContractMatrix, EmptyQueryValuesAreValidatedNotIgnored) {
  const ApiClient user(*server_, *user_);
  for (const char* path : {"/calendar?to=", "/tasks?due_before=", "/tasks?recurring=", "/tasks?limit=",
                           "/detections?from="}) {
    EXPECT_EQ(user.get(path).status, 422) << path;
  }
}

}  // namespace
