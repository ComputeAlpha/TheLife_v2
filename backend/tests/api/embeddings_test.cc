// Semantic memory end to end: embeddings.sync through a (fake) thelife-embedder, hybrid history search, semantic
// fact recall, related-from-earlier context, and the fallback when the embedder is down.
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "core/ids.h"
#include "core/json.h"
#include "support/api_client.h"
#include "support/fake_llm.h"

using namespace thelife;
using namespace thelife::test;

namespace {

Json::Value J(const std::string& t) { return json::parse(t); }

drogon::orm::Result sql(const std::string& tenant, const std::string& q, const std::vector<std::string>& args = {}) {
  auto tx = appDb()->newTransaction();
  tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenant);
  auto r = args.empty() ? tx->execSqlSync(q)
           : args.size() == 1 ? tx->execSqlSync(q, args[0])
                              : tx->execSqlSync(q, args[0], args[1]);
  std::promise<bool> committed;
  tx->setCommitCallback([&committed](bool ok) { committed.set_value(ok); });
  tx.reset();
  EXPECT_TRUE(committed.get_future().get());
  return r;
}

template <typename F>
bool eventually(F cond, int ms = 10000) {
  for (int i = 0; i < ms / 50; ++i) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return cond();
}

class Semantic : public ::testing::Test {
 protected:
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<FakeEmbedder> embedder;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  TestUser user;

  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    fake = std::make_unique<FakeLlm>();
    embedder = std::make_unique<FakeEmbedder>();
    user = createTenantWithUser();
    server = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake->baseUrl()},
                                {"THELIFE_EMBEDDER_SOCKET", embedder->socket()},
                                {"THELIFE_RECALL_MIN_SCORE", "0.1"},  // fake vectors: unrelated texts score about ±0.03
                                {"THELIFE_JOBS", "on"},
                                {"THELIFE_JOBS_TENANT", user.tenantId},
                                {"THELIFE_JOBS_HOUSEKEEPING", "off"},
                                {"THELIFE_JOBS_POLL_MS", "100"}});
    api = std::make_unique<ApiClient>(*server, user);
  }

  Json::Value chat(const std::string& text) {
    Json::Value b;
    b["text"] = text;
    auto t = api->post("/chat/web/messages", b).json;
    for (int i = 0; i < 200; ++i) {
      t = api->get("/chat/turns/" + t["id"].asString()).json;
      if (t["status"].asString() != "queued" && t["status"].asString() != "running") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return t;
  }

  int embedded(const std::string& type) {
    return sql(user.tenantId, "SELECT count(DISTINCT owner_id)::int AS n FROM embeddings WHERE owner_type = $1",
               {type})[0]["n"].as<int>();
  }

  Json::Value lastToolResult() {
    const auto convs = api->get("/conversations").json["items"];
    const auto conv = convs[convs.size() - 1]["id"].asString();  // today's (the list is oldest first)
    const auto items = api->get("/conversations/" + conv + "/messages").json["items"];
    Json::Value last;
    for (const auto& m : items) {
      if (m["role"].asString() == "tool") last = json::parse(m["content"].asString());
    }
    return last;
  }

  void queueSync() {
    sql(user.tenantId,
        "INSERT INTO ops.jobs (id, tenant_id, kind, dedupe_key) VALUES (gen_random_uuid(), $1::uuid, "
        "'embeddings.sync', 'tenant:' || $1) ON CONFLICT DO NOTHING",
        {user.tenantId});
  }
};

}  // namespace

TEST_F(Semantic, TurnsAreEmbeddedAndHistorySearchFindsByMeaning) {
  fake->script(J(R"([{"content":"Logged your gym session with squats."},
    {"content":"","tool_calls":[{"name":"history_search","arguments":{"query":"exercise"}}]},
    {"content":"You trained on Friday."}])"));
  chat("Did a 45 minute gym session with heavy squats");
  ASSERT_TRUE(eventually([&] { return embedded("message") == 2; })) << "turn messages were not embedded";
  const auto row = sql(user.tenantId, "SELECT model, model_version, vector_dims(embedding) AS d FROM embeddings LIMIT 1");
  EXPECT_EQ(row[0]["model_version"].as<std::string>(), "fake-1");
  EXPECT_EQ(row[0]["d"].as<int>(), 1024);

  chat("when did I last exercise?");
  const auto r = lastToolResult();
  bool semantic = false;
  for (const auto& i : r["items"]) {
    if (i["text"].asString().find("heavy squats") != std::string::npos) semantic = i["match"].asString() == "semantic";
  }
  EXPECT_TRUE(semantic) << r;  // no shared word, found by meaning ("exercise" ~ "gym")
}

TEST_F(Semantic, ChangedItemsAreEmbeddedAgain) {
  const auto a = api->post("/activities", J(R"({"title":"Dentist visit","area":"personal",
      "started_at":"2026-11-10T10:00:00+05:30","ended_at":"2026-11-10T11:00:00+05:30"})"));
  ASSERT_EQ(a.status, 201);
  queueSync();
  ASSERT_TRUE(eventually([&] { return embedded("activity") == 1; }));
  const auto before = sql(user.tenantId, "SELECT content_hash FROM embeddings WHERE owner_type = 'activity'")[0][0].as<std::string>();
  ASSERT_EQ(api->patch("/activities/" + a.json["id"].asString(), J(R"({"title":"Dentist visit, filling done"})"), "\"1\"").status, 200);
  queueSync();
  ASSERT_TRUE(eventually([&] {
    const auto r = sql(user.tenantId, "SELECT content_hash FROM embeddings WHERE owner_type = 'activity'");
    return r.size() == 1 && r[0][0].as<std::string>() != before;
  }));
}

TEST_F(Semantic, RelatedEarlierItemsReachTheContextAndFactsAreRecalledByMeaning) {
  // A conversation from ten days ago, outside the verbatim window.
  const auto conv = ids::newUuidV7();
  sql(user.tenantId,
      "INSERT INTO conversations (id, tenant_id, created_by_kind, user_id, channel, local_day) "
      "SELECT $1::uuid, tenant_id, 'user', id, 'web', current_date - 10 FROM users",
      {conv});
  sql(user.tenantId,
      "INSERT INTO messages (id, tenant_id, conversation_id, role, content, created_at) "
      "SELECT $1::uuid, tenant_id, $2::uuid, 'user', 'My knee hurt after the gym, going lighter next time', "
      "now() - interval '10 days' FROM users",
      {ids::newUuidV7(), conv});
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"memory_remember","arguments":{"subject":"me",
      "statement":"Has a dentist appointment on Monday at 9"}}]},{"content":"Noted."}])"));
  chat("Remember: dentist on Monday at 9");
  ASSERT_TRUE(eventually([&] { return embedded("message") >= 2 && embedded("fact") == 1; }));

  fake->script(J(R"([{"content":"Plan a lighter session."},
    {"content":"","tool_calls":[{"name":"memory_recall","arguments":{"query":"doctor"}}]},{"content":"Monday."}])"));
  chat("planning tomorrow's workout");
  std::string sys;
  const auto msgs = fake->requests()[2]["messages"];
  for (const auto& m : msgs) {
    if (m["role"].asString() == "system") sys += m["content"].asString() + "\n";
  }
  EXPECT_NE(sys.find("Related from earlier (by meaning):"), std::string::npos) << sys;
  EXPECT_NE(sys.find("My knee hurt after the gym"), std::string::npos) << sys;

  chat("when do I see the doctor?");
  const auto recall = lastToolResult();
  ASSERT_EQ(recall["items"].size(), 1u) << recall;  // keyword "doctor" matches nothing; meaning does
  EXPECT_EQ(recall["items"][0]["statement"].asString(), "Has a dentist appointment on Monday at 9");
}

TEST_F(Semantic, WithoutTheEmbedderSearchFallsBackAndSyncWaits) {
  embedder->stop();
  fake->script(J(R"([{"content":"Logged your squats."},
    {"content":"","tool_calls":[{"name":"history_search","arguments":{"query":"squats"}}]},{"content":"Friday."}])"));
  chat("Heavy squats today, 5x5");
  chat("when did I do squats?");
  const auto r = lastToolResult();
  ASSERT_GE(r["items"].size(), 1u);
  for (const auto& i : r["items"]) EXPECT_EQ(i["match"].asString(), "keyword");
  ASSERT_TRUE(eventually([&] {
    const auto j = sql(user.tenantId, "SELECT last_error FROM ops.jobs WHERE tenant_id = $1::uuid AND kind = 'embeddings.sync'",
                       {user.tenantId});
    return !j.empty() && !j[0][0].isNull();
  }));
  const auto j = sql(user.tenantId,
                     "SELECT status, attempts, last_error FROM ops.jobs WHERE tenant_id = $1::uuid AND kind = 'embeddings.sync'",
                     {user.tenantId})[0];
  EXPECT_EQ(j["status"].as<std::string>(), "queued");
  EXPECT_EQ(j["attempts"].as<int>(), 0);
  EXPECT_EQ(j["last_error"].as<std::string>(), "embedder unavailable");
  EXPECT_EQ(embedded("message"), 0);
}
