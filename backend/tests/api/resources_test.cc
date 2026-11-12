// Every contract CRUD resource through the same lifecycle and cross-tenant checks. A new resource with
// create/update in the manifest fails EveryCrudResourceHasACase until a case is added here.
#include <gtest/gtest.h>

#include <functional>
#include <set>

#include "core/json.h"
#include "support/api_client.h"
#include "tools/manifest.h"

using namespace thelife::test;
using thelife::json::parse;

namespace {

struct Case {
  std::string prefix;
  // Builds a valid create body; may create parents through the client first.
  std::function<Json::Value(const ApiClient&)> body;
  std::string patch;  // JSON merge patch that must succeed on the created item
};

std::string idOf(const HttpResult& r) {
  if (r.status != 201) throw std::runtime_error("parent create failed: " + r.body);
  return r.json["id"].asString();
}

Json::Value with(const char* json, const std::string& key, const std::string& id) {
  auto v = parse(json);
  v[key] = id;
  return v;
}

const std::vector<Case>& cases() {
  static const std::vector<Case> all = {
      {"goals", [](auto&) { return parse(R"({"title":"g","area":"skill","horizon":"year"})"); }, R"({"status":"paused"})"},
      {"plans", [](auto&) { return parse(R"({"title":"p","period_start":"2026-01-01","period_end":"2026-01-31"})"); },
       R"({"status":"done"})"},
      {"tasks", [](auto&) { return parse(R"({"title":"t","area":"work","rrule":"FREQ=DAILY"})"); },
       R"({"priority":3,"rrule":null})"},
      {"events",
       [](auto&) {
         return parse(R"({"title":"e","starts_at":"2026-11-12T09:00:00+00:00","ends_at":"2026-11-12T10:00:00+00:00"})");
       },
       R"({"location":"room 4","category":"routine.gym"})"},
      {"activities",
       [](auto&) {
         return parse(R"({"area":"fitness","title":"run","started_at":"2026-11-12T06:00:00+00:00",
                          "ended_at":"2026-11-12T06:40:00+00:00"})");
       },
       R"({"notes":"easy pace"})"},
      {"work_roles", [](auto&) { return parse(R"({"title":"Engineer","organization":"Acme"})"); },
       R"({"active":false})"},
      {"work_responsibilities",
       [](const ApiClient& api) {
         return with(R"({"title":"on-call","cadence":"FREQ=WEEKLY;BYDAY=MO"})", "role_id",
                     idOf(api.post("/work/roles", parse(R"({"title":"r"})"))));
       },
       R"({"active":false})"},
      {"work_projects", [](auto&) { return parse(R"({"title":"proj","starts_on":"2026-01-01"})"); },
       R"({"status":"on_hold"})"},
      {"work_deliverables",
       [](const ApiClient& api) {
         return with(R"({"title":"spec"})", "project_id", idOf(api.post("/work/projects", parse(R"({"title":"p"})"))));
       },
       R"({"status":"in_progress"})"},
      {"skill_resources", [](auto&) { return parse(R"({"kind":"book","title":"SICP","total_pages":600})"); },
       R"({"status":"active"})"},
      {"skill_sections",
       [](const ApiClient& api) {
         return with(R"({"ordinal":0,"title":"intro","duration_s":600})", "resource_id",
                     idOf(api.post("/skills/resources", parse(R"({"kind":"course","title":"c"})"))));
       },
       R"({"title":"Introduction"})"},
      {"skill_sessions",
       [](const ApiClient& api) {
         return with(R"({"started_at":"2026-11-12T20:00:00+00:00","ended_at":"2026-11-12T21:00:00+00:00",
                         "page_from":1,"page_to":30})",
                     "resource_id", idOf(api.post("/skills/resources", parse(R"({"kind":"book","title":"b"})"))));
       },
       R"({"notes":"chapter 1"})"},
      {"entertainment_items", [](auto&) { return parse(R"({"kind":"game","title":"x","meta":{"platform":"pc"}})"); },
       R"({"status":"playing"})"},
      {"entertainment_slots",
       [](auto&) { return parse(R"({"starts_at":"2026-11-14T18:00:00+00:00","ends_at":"2026-11-14T20:00:00+00:00"})"); },
       R"({"consumed":true})"},
  };
  return all;
}

std::string collectionPath(const std::string& prefix) {
  const auto& tools = thelife::tools::embeddedManifest().tools;
  return tools.at(prefix + ".create").rest->path.substr(std::string("/api/v1").size());
}

TEST(ResourceMatrix, EveryCrudResourceHasACase) {
  std::set<std::string> covered;
  for (const auto& c : cases()) covered.insert(c.prefix);
  for (const auto& [name, def] : thelife::tools::embeddedManifest().tools) {
    if (def.stage > 1 || !name.ends_with(".update")) continue;
    const auto prefix = name.substr(0, name.size() - std::string(".update").size());
    EXPECT_TRUE(covered.count(prefix)) << prefix << " has no case in resources_test.cc";
  }
}

class ResourceLifecycle : public ::testing::TestWithParam<Case> {
 protected:
  static void SetUpTestSuite() {
    if (dbAvailable()) server_ = startServerWithDb().release();
  }
  static void TearDownTestSuite() {
    delete server_;
    server_ = nullptr;
  }
  void SetUp() override { THELIFE_SKIP_WITHOUT_DB(); }
  static ServerProcess* server_;
};
ServerProcess* ResourceLifecycle::server_ = nullptr;

TEST_P(ResourceLifecycle, CreateGetListPatchDeleteAndIsolation) {
  const auto& c = GetParam();
  const ApiClient alice(*server_, createTenantWithUser());
  const ApiClient bob(*server_, createTenantWithUser());
  const auto path = collectionPath(c.prefix);

  const auto body = c.body(alice);
  auto created = alice.post(path, body);
  ASSERT_EQ(created.status, 201) << created.body;
  EXPECT_EQ(created.header("etag"), "\"1\"");
  EXPECT_EQ(created.json["created_by"]["kind"].asString(), "user");
  EXPECT_FALSE(created.json.isMember("tenant_id"));
  for (const auto& k : body.getMemberNames()) EXPECT_EQ(created.json[k], body[k]) << c.prefix << "." << k;
  const auto id = created.json["id"].asString();
  const auto item = path + "/" + id;

  auto got = alice.get(item);
  ASSERT_EQ(got.status, 200) << got.body;
  EXPECT_EQ(got.json, created.json);
  auto list = alice.get(path);
  ASSERT_EQ(list.status, 200) << list.body;
  bool listed = false;
  for (const auto& i : list.json["items"]) listed |= i["id"].asString() == id;
  EXPECT_TRUE(listed);

  // another tenant: invisible, unchangeable, unlistable
  EXPECT_EQ(bob.get(item).status, 404);
  EXPECT_EQ(bob.patch(item, parse(c.patch), "\"1\"").status, 404);
  EXPECT_EQ(bob.del(item, "\"1\"").status, 404);
  EXPECT_EQ(bob.get(path).json["items"].size(), 0u);

  EXPECT_EQ(alice.patch(item, parse(c.patch), "").status, 428);
  auto patched = alice.patch(item, parse(c.patch), "\"1\"");
  ASSERT_EQ(patched.status, 200) << patched.body;
  EXPECT_EQ(patched.json["version"].asInt(), 2);
  const auto patch = parse(c.patch);
  for (const auto& k : patch.getMemberNames()) EXPECT_EQ(patched.json[k], patch[k]) << c.prefix << "." << k;

  EXPECT_EQ(alice.del(item, "\"1\"").status, 412);
  EXPECT_EQ(alice.del(item, "\"2\"").status, 204);
  EXPECT_EQ(alice.get(item).status, 404);
}

INSTANTIATE_TEST_SUITE_P(Stage1, ResourceLifecycle, ::testing::ValuesIn(cases()),
                         [](const auto& p) { return p.param.prefix; });

}  // namespace
