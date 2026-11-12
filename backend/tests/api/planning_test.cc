// Goals and plans through the generic tool → REST path (contract-driven CRUD).
#include <gtest/gtest.h>

#include "core/json.h"
#include "support/api_client.h"

using namespace thelife::test;
using thelife::json::parse;

namespace {

class PlanningApi : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    server_ = startServerWithDb();
    alice_ = std::make_unique<ApiClient>(*server_, createTenantWithUser());
    bob_ = std::make_unique<ApiClient>(*server_, createTenantWithUser());
  }
  Json::Value goal(const std::string& title = "Read 12 books") {
    return parse(R"({"title":")" + title + R"(","area":"skill","horizon":"year"})");
  }
  std::unique_ptr<ServerProcess> server_;
  std::unique_ptr<ApiClient> alice_, bob_;
};

TEST_F(PlanningApi, CreateReturnsResourceWithMetaAndEtag) {
  auto r = alice_->post("/goals", goal());
  ASSERT_EQ(r.status, 201) << r.body;
  EXPECT_EQ(r.header("etag"), "\"1\"");
  EXPECT_EQ(r.json["version"].asInt(), 1);
  EXPECT_EQ(r.json["status"].asString(), "active");  // contract default applied
  EXPECT_EQ(r.json["created_by"]["kind"].asString(), "user");
  EXPECT_EQ(r.json["created_by"]["id"].asString(), alice_->user().userId);
  EXPECT_EQ(r.json["origin"].asString(), "direct");
  EXPECT_FALSE(r.json.isMember("tenant_id"));  // internal columns never leave
  EXPECT_FALSE(r.json.isMember("created_by_kind"));
}

TEST_F(PlanningApi, GetPatchDeleteLifecycleWithPreconditions) {
  const auto id = alice_->post("/goals", goal()).json["id"].asString();
  EXPECT_EQ(alice_->get("/goals/" + id).status, 200);

  auto noIfMatch = alice_->patch("/goals/" + id, parse(R"({"status":"paused"})"), "");
  EXPECT_EQ(noIfMatch.status, 428);
  EXPECT_EQ(alice_->patch("/goals/" + id, parse(R"({"status":"paused"})"), "\"9\"").status, 412);
  EXPECT_EQ(alice_->patch("/goals/" + id, parse(R"({"status":"paused"})"), "3").status, 422);  // unquoted

  auto ok = alice_->patch("/goals/" + id, parse(R"({"status":"paused","description":"slow year"})"), "\"1\"");
  ASSERT_EQ(ok.status, 200) << ok.body;
  EXPECT_EQ(ok.json["version"].asInt(), 2);
  EXPECT_EQ(ok.json["status"].asString(), "paused");
  EXPECT_EQ(ok.json["title"].asString(), "Read 12 books");  // untouched fields stay

  // merge-patch null clears a nullable field; nulling a required one is rejected
  EXPECT_EQ(alice_->patch("/goals/" + id, parse(R"({"description":null})"), "\"2\"").json["description"],
            Json::Value());
  EXPECT_EQ(alice_->patch("/goals/" + id, parse(R"({"title":null})"), "\"3\"").status, 422);

  EXPECT_EQ(alice_->del("/goals/" + id, "").status, 428);
  EXPECT_EQ(alice_->del("/goals/" + id, "\"1\"").status, 412);
  EXPECT_EQ(alice_->del("/goals/" + id, "\"3\"").status, 204);
  EXPECT_EQ(alice_->get("/goals/" + id).status, 404);
  EXPECT_EQ(alice_->del("/goals/" + id, "\"3\"").status, 404);
  // still visible to sync clients
  auto all = alice_->get("/goals?include_deleted=true");
  ASSERT_EQ(all.json["items"].size(), 1u);
  EXPECT_FALSE(all.json["items"][0]["deleted_at"].isNull());
}

TEST_F(PlanningApi, ValidationErrorsPointAtFields) {
  auto r = alice_->post("/goals", parse(R"({"title":"","area":"nap","horizon":"year","bogus":1})"));
  ASSERT_EQ(r.status, 422);
  std::set<std::string> paths;
  for (const auto& e : r.json["errors"]) paths.insert(e["path"].asString());
  EXPECT_TRUE(paths.count("/title") && paths.count("/area") && paths.count("/bogus")) << r.body;
  EXPECT_EQ(alice_->get("/goals/not-a-uuid").status, 422);
  EXPECT_EQ(alice_->get("/goals?limit=0").status, 422);
  EXPECT_EQ(alice_->get("/goals?limit=abc").status, 422);
  EXPECT_EQ(alice_->get("/goals?cursor=@@@").status, 422);
  // database-level rule: due before start
  EXPECT_EQ(alice_->post("/goals", parse(R"({"title":"x","area":"skill","horizon":"year",
                                             "starts_on":"2026-06-01","due_on":"2026-01-01"})")).status,
            422);
}

TEST_F(PlanningApi, ListPaginatesWithCursorAndFilters) {
  for (int i = 0; i < 3; ++i) alice_->post("/goals", goal("g" + std::to_string(i)));
  alice_->post("/goals", parse(R"({"title":"run","area":"fitness","horizon":"month"})"));

  auto page1 = alice_->get("/goals?limit=2");
  ASSERT_EQ(page1.json["items"].size(), 2u);
  ASSERT_TRUE(page1.json["next_cursor"].isString());
  auto page2 = alice_->get("/goals?limit=2&cursor=" + page1.json["next_cursor"].asString());
  ASSERT_EQ(page2.json["items"].size(), 2u);
  EXPECT_TRUE(page2.json["next_cursor"].isNull());
  EXPECT_LT(page1.json["items"][1]["id"].asString(), page2.json["items"][0]["id"].asString());

  EXPECT_EQ(alice_->get("/goals?area=fitness").json["items"].size(), 1u);
  EXPECT_EQ(alice_->get("/goals?horizon=year&area=skill").json["items"].size(), 3u);
  EXPECT_EQ(alice_->get("/goals?area=bogus").status, 422);
}

TEST_F(PlanningApi, OtherTenantsDataIsInvisible) {
  const auto id = alice_->post("/goals", goal()).json["id"].asString();
  EXPECT_EQ(bob_->get("/goals/" + id).status, 404);
  EXPECT_EQ(bob_->patch("/goals/" + id, parse(R"({"status":"paused"})"), "\"1\"").status, 404);
  EXPECT_EQ(bob_->del("/goals/" + id, "\"1\"").status, 404);
  EXPECT_EQ(bob_->get("/goals").json["items"].size(), 0u);
  // cannot link to another tenant's goal either (composite foreign key)
  auto link = bob_->post("/plans", parse(R"({"title":"p","period_start":"2026-01-01","period_end":"2026-12-31",
                                             "goal_id":")" + id + "\"}"));
  EXPECT_EQ(link.status, 422) << link.body;
  EXPECT_EQ(alice_->get("/goals/" + id).json["version"].asInt(), 1);
}

TEST_F(PlanningApi, IdempotencyKeyReplaysAndRejectsReuse) {
  auto first = alice_->post("/goals", goal(), "key-0000001");
  ASSERT_EQ(first.status, 201);
  auto again = alice_->post("/goals", goal(), "key-0000001");
  EXPECT_EQ(again.status, 201);
  EXPECT_EQ(again.json["id"], first.json["id"]);
  EXPECT_EQ(again.header("idempotent-replayed"), "true");
  EXPECT_EQ(alice_->get("/goals").json["items"].size(), 1u);
  EXPECT_EQ(alice_->post("/goals", goal("different"), "key-0000001").status, 422);
  // keys are per principal: bob's identical key is independent
  EXPECT_EQ(bob_->post("/goals", goal(), "key-0000001").status, 201);
  EXPECT_EQ(alice_->post("/goals", goal(), "short").status, 422);
}

TEST_F(PlanningApi, PlansActiveOnFilterAndGoalLink) {
  const auto gid = alice_->post("/goals", goal()).json["id"].asString();
  auto p = alice_->post("/plans", parse(R"({"title":"Q1","period_start":"2026-01-01","period_end":"2026-03-31",
                                           "goal_id":")" + gid + "\"}"));
  ASSERT_EQ(p.status, 201) << p.body;
  alice_->post("/plans", parse(R"({"title":"Q2","period_start":"2026-04-01","period_end":"2026-06-30"})"));
  auto feb = alice_->get("/plans?active_on=2026-02-14");
  ASSERT_EQ(feb.json["items"].size(), 1u) << feb.body;
  EXPECT_EQ(feb.json["items"][0]["title"].asString(), "Q1");
  EXPECT_EQ(alice_->get("/plans?goal_id=" + gid).json["items"].size(), 1u);
  EXPECT_EQ(alice_->post("/plans", parse(R"({"title":"bad","period_start":"2026-05-01","period_end":"2026-04-01"})"))
                .status,
            422);
}

}  // namespace
