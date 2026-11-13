// Memory (agent_lld.md §6) and background jobs (backend_lld.md §10) end to end: the real binary, docker/fake-llm,
// and a job worker scoped to the test's tenant (THELIFE_JOBS_TENANT) so parallel tests never take each other's jobs.
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

Json::Value J(const std::string& text) { return json::parse(text); }

drogon::orm::Result sql(const std::string& tenant, const std::string& q, const std::vector<std::string>& args = {}) {
  auto tx = appDb()->newTransaction();
  tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenant);
  auto r = args.empty() ? tx->execSqlSync(q)
           : args.size() == 1 ? tx->execSqlSync(q, args[0])
           : args.size() == 2 ? tx->execSqlSync(q, args[0], args[1])
                              : tx->execSqlSync(q, args[0], args[1], args[2]);
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

class Memory : public ::testing::Test {
 protected:
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  TestUser user;

  void start(bool jobs) {
    THELIFE_SKIP_WITHOUT_DB();
    fake = std::make_unique<FakeLlm>();
    user = createTenantWithUser();
    std::map<std::string, std::string> env{{"THELIFE_LLM_BASE_URL", fake->baseUrl()}};
    if (jobs) {
      env["THELIFE_JOBS"] = "on";
      env["THELIFE_JOBS_TENANT"] = user.tenantId;
      env["THELIFE_JOBS_HOUSEKEEPING"] = "off";
      env["THELIFE_JOBS_POLL_MS"] = "100";
    }
    server = startServerWithDb(env);
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

  std::string day(int offset) {
    return sql(user.tenantId,
               "SELECT to_char(((now() AT TIME ZONE timezone) - make_interval(hours => day_start_hour))::date + $1::int,"
               " 'YYYY-MM-DD') AS d FROM users",
               {std::to_string(offset)})[0]["d"].as<std::string>();
  }

  // Concatenated system messages of the n-th model request.
  std::string systemText(int n) {
    std::string out;
    const auto msgs = fake->requests()[n]["messages"];
    for (const auto& m : msgs) {
      if (m["role"].asString() == "system") out += m["content"].asString() + "\n";
    }
    return out;
  }

  std::string toolResult(int requestIndex) {
    const auto msgs = fake->requests()[requestIndex]["messages"];
    return msgs[msgs.size() - 1]["content"].asString();
  }
};

}  // namespace

TEST_F(Memory, RememberedFactsComeBackInContextAndRecall) {
  start(false);
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"memory_remember","arguments":{"subject":"Priya","subject_kind":"person",
      "statement":"Priya is the user's manager at Acme"}}]},
    {"content":"Noted: Priya is your manager."},
    {"content":"Good luck with the meeting."},
    {"content":"","tool_calls":[{"name":"memory_recall","arguments":{"query":"Priya"}}]},
    {"content":"She is your manager."}])"));
  EXPECT_EQ(chat("Priya is my manager at Acme")["status"].asString(), "done");
  const auto facts = api->get("/memory/facts").json["items"];
  ASSERT_EQ(facts.size(), 1u);
  EXPECT_EQ(facts[0]["statement"].asString(), "Priya is the user's manager at Acme");
  EXPECT_EQ(facts[0]["origin"].asString(), "user_stated");
  EXPECT_EQ(facts[0]["created_by"]["kind"].asString(), "agent");
  EXPECT_EQ(facts[0]["status"].asString(), "active");

  chat("Meeting Priya tomorrow at 10");
  const auto sys = systemText(2);
  EXPECT_NE(sys.find("What you know about"), std::string::npos) << sys;
  EXPECT_NE(sys.find("Remembered about things in the latest message:\n- Priya: Priya is the user's manager"),
            std::string::npos)
      << sys;

  chat("who is priya again?");
  EXPECT_NE(toolResult(4).find("manager at Acme"), std::string::npos);
  EXPECT_FALSE(api->get("/memory/facts").json["items"][0]["last_used_at"].isNull());

  // Saying the same thing twice does not duplicate the fact.
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"memory_remember","arguments":{"subject":"priya",
      "statement":"Priya is the user's manager at Acme"}}]},{"content":"Already known."}])"));
  chat("remember Priya is my manager at Acme");
  EXPECT_EQ(api->get("/memory/facts").json["items"].size(), 1u);
}

TEST_F(Memory, UpdatingAndForgettingFollowProvenance) {
  start(false);
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"memory_remember","arguments":{"subject":"me","statement":"Prefers deep work before noon"}}]},
    {"content":"Noted."}])"));
  chat("I do my best deep work before noon");
  const auto stated = api->get("/memory/facts").json["items"][0]["id"].asString();

  // Correcting a fact the user stated waits for approval; approving supersedes it.
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"memory_update_fact","arguments":{"id":")" + stated +
                 R"(","statement":"Prefers deep work before 11"}}]},{"content":"Needs approval."},{"content":"Updated."}])"));
  chat("actually make that before 11");
  const auto pending = api->get("/approvals?status=pending").json["items"];
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0]["tool"].asString(), "memory.update_fact");
  const auto decided = api->post("/approvals/" + pending[0]["id"].asString() + ":approve", Json::Value(Json::objectValue));
  ASSERT_EQ(decided.json["status"].asString(), "approved") << decided.body;
  EXPECT_EQ(api->get("/memory/facts/" + stated).json["status"].asString(), "superseded");
  const auto active = api->get("/memory/facts?status=active").json["items"];
  ASSERT_EQ(active.size(), 1u);
  EXPECT_EQ(active[0]["statement"].asString(), "Prefers deep work before 11");
  EXPECT_EQ(api->get("/memory/facts/" + stated).json["superseded_by"].asString(), active[0]["id"].asString());
  ASSERT_TRUE(eventually([&] {
    return sql(user.tenantId, "SELECT count(*) AS n FROM turns WHERE trigger = 'approval' AND status = 'done'")[0]["n"].as<int>() == 1;
  }));

  // An inferred fact the agent wrote may be forgotten without approval; a stated one may not.
  const auto inferredId = ids::newUuidV7();
  sql(user.tenantId,
      "INSERT INTO kg_facts (id, tenant_id, created_by_kind, origin, user_id, subject_id, statement, confidence) "
      "SELECT $1::uuid, f.tenant_id, 'agent', 'inferred', f.user_id, f.subject_id, 'Often skips lunch', 0.8 "
      "FROM kg_facts f WHERE f.id = $2::uuid",
      {inferredId, stated});
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"memory_forget","arguments":{"id":")" + inferredId + R"("}},
                     {"name":"memory_forget","arguments":{"id":")" + active[0]["id"].asString() + R"("}}]},
                    {"content":"Forgot one; the other needs approval."}])"));
  chat("forget the lunch thing and the deep work thing");
  EXPECT_EQ(api->get("/memory/facts/" + inferredId).status, 404);
  EXPECT_EQ(api->get("/memory/facts/" + active[0]["id"].asString()).status, 200);
  const auto forget = api->get("/approvals?status=pending").json["items"];
  ASSERT_EQ(forget.size(), 1u);
  EXPECT_EQ(forget[0]["tool"].asString(), "memory.forget");

  // The user can always delete a fact directly.
  const auto f = api->get("/memory/facts/" + active[0]["id"].asString()).json;
  EXPECT_EQ(api->del("/memory/facts/" + f["id"].asString(), "\"" + std::to_string(f["version"].asInt()) + "\"").status, 204);
  EXPECT_EQ(api->get("/memory/facts?status=active").json["items"].size(), 0u);
}

TEST_F(Memory, HistorySearchFindsMessagesAndActivities) {
  start(false);
  ASSERT_EQ(api->post("/activities", J(R"({"title":"Flamingo design review","area":"work",
      "started_at":"2026-11-10T10:00:00+05:30","ended_at":"2026-11-10T11:00:00+05:30"})")).status, 201);
  fake->script(J(R"([{"content":"Noted the flamingo budget."},
                    {"content":"","tool_calls":[{"name":"history_search","arguments":{"query":"flamingo"}}]},
                    {"content":"Found it."}])"));
  chat("the flamingo budget is 40k");
  chat("what did we say about flamingo?");
  const auto result = json::parse(toolResult(2));
  std::set<std::string> kinds;
  for (const auto& i : result["items"]) kinds.insert(i["kind"].asString());
  EXPECT_TRUE(kinds.count("message")) << result;
  EXPECT_TRUE(kinds.count("activity")) << result;
}

TEST_F(Memory, DayCloseSummarisesExtractsFactsClosesAndMarksMissed) {
  start(true);
  const auto yesterday = day(-1);
  const auto routine = api->post("/tasks", J(R"({"title":"Meditate","area":"personal","rrule":"FREQ=DAILY",
      "rrule_dtstart":"2026-01-01T00:00:00+00:00"})"));
  ASSERT_EQ(routine.status, 201) << routine.body;
  const auto conv = ids::newUuidV7();
  sql(user.tenantId,
      "INSERT INTO conversations (id, tenant_id, created_by_kind, user_id, channel, local_day) "
      "SELECT $1::uuid, tenant_id, 'user', id, 'web', $2::date FROM users",
      {conv, yesterday});
  sql(user.tenantId,
      "INSERT INTO messages (id, tenant_id, conversation_id, role, content) "
      "SELECT $1::uuid, tenant_id, $2::uuid, 'user', 'Started at Acme as a backend engineer today' FROM users",
      {ids::newUuidV7(), conv});
  sql(user.tenantId,
      "INSERT INTO messages (id, tenant_id, conversation_id, role, content) "
      "SELECT $1::uuid, tenant_id, $2::uuid, 'assistant', 'Congratulations on the new job!' FROM users",
      {ids::newUuidV7(), conv});
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"submit_day_summary","arguments":{
      "summary":"User started a new job at Acme as a backend engineer.","topics":["work","new job"],
      "facts":[{"subject":"me","statement":"Works at Acme as a backend engineer","confidence":0.95},
               {"subject":"me","statement":"Might move to the night shift","confidence":0.4}]}}]}])"));

  // Scheduling: yesterday is closed once per user.
  sql(user.tenantId, "SELECT ops.enqueue_day_closes()");
  sql(user.tenantId, "SELECT ops.enqueue_day_closes()");
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM ops.day_closes WHERE user_id = $1::uuid", {user.userId})[0]["n"].as<int>(), 1);

  ASSERT_TRUE(eventually([&] {
    return sql(user.tenantId, "SELECT count(*) AS n FROM episodic_summaries")[0]["n"].as<int>() == 1;
  })) << "no day summary";
  const auto s = sql(user.tenantId, "SELECT to_char(period_start, 'YYYY-MM-DD') AS d, content, period, channel, "
                                    "array_to_string(topics, ',') AS topics FROM episodic_summaries");
  EXPECT_EQ(s[0]["d"].as<std::string>(), yesterday);
  EXPECT_EQ(s[0]["period"].as<std::string>(), "day");
  EXPECT_EQ(s[0]["topics"].as<std::string>(), "work,new job");

  EXPECT_EQ(api->get("/memory/facts?status=active").json["items"][0]["statement"].asString(),
            "Works at Acme as a backend engineer");
  EXPECT_EQ(api->get("/memory/facts?status=proposed").json["items"][0]["statement"].asString(),
            "Might move to the night shift");
  EXPECT_EQ(api->get("/conversations/" + conv).json["status"].asString(), "closed");
  const auto occ = api->get("/tasks/" + routine.json["id"].asString() + "/occurrences?from=" + yesterday + "&to=" + yesterday);
  ASSERT_EQ(occ.json["items"].size(), 1u) << occ.body;
  EXPECT_EQ(occ.json["items"][0]["status"].asString(), "missed");

  const auto req = fake->requests()[0];
  EXPECT_EQ(req["reasoning_effort"].asString(), "low");
  EXPECT_NE(req["messages"][0]["content"].asString().find("maintain the memory"), std::string::npos);
  EXPECT_NE(req["messages"][1]["content"].asString().find("User: Started at Acme"), std::string::npos);
  const auto jobs = sql(user.tenantId, "SELECT kind, status FROM ops.jobs WHERE tenant_id = $1::uuid AND kind IN ('day.close', 'memory.compress_day') ORDER BY created_at",
                        {user.tenantId});
  ASSERT_EQ(jobs.size(), 2u);
  EXPECT_EQ(jobs[0]["kind"].as<std::string>(), "day.close");
  EXPECT_EQ(jobs[1]["status"].as<std::string>(), "done");
  const auto usage = sql(user.tenantId, "SELECT purpose FROM llm_calls");
  EXPECT_EQ(usage[0]["purpose"].as<std::string>(), "memory.compress_day");

  // Earlier day summaries and the extracted profile reach the agent's context.
  sql(user.tenantId,
      "INSERT INTO episodic_summaries (id, tenant_id, user_id, channel, period, period_start, content, model, "
      "prompt_version) SELECT $1::uuid, tenant_id, id, 'web', 'day', $2::date, 'Planned the flamingo launch.', 'm', 'v' "
      "FROM users",
      {ids::newUuidV7(), day(-3)});
  fake->script(J(R"([{"content":"Hi!"}])"));
  chat("hi");
  const auto sys = systemText(1);
  EXPECT_NE(sys.find("Earlier days on this channel (summaries):\n" + day(-3) + ": Planned the flamingo launch."),
            std::string::npos)
      << sys;
  EXPECT_NE(sys.find("- Works at Acme as a backend engineer"), std::string::npos);
  EXPECT_EQ(sys.find("night shift"), std::string::npos);  // proposed facts are not used
}

TEST_F(Memory, JobsWaitForTheQuotaResetInsteadOfFailing) {
  start(true);
  const auto conv = ids::newUuidV7();
  const auto yesterday = day(-1);
  sql(user.tenantId,
      "INSERT INTO conversations (id, tenant_id, created_by_kind, user_id, channel, local_day) "
      "SELECT $1::uuid, tenant_id, 'user', id, 'web', $2::date FROM users",
      {conv, yesterday});
  sql(user.tenantId,
      "INSERT INTO messages (id, tenant_id, conversation_id, role, content) "
      "SELECT $1::uuid, tenant_id, $2::uuid, 'user', 'hello' FROM users",
      {ids::newUuidV7(), conv});
  fake->script(J(R"([{"error":{"status":429,"body":{"error":"weekly usage limit reached"}}}])"));
  sql(user.tenantId,
      "INSERT INTO ops.jobs (id, tenant_id, kind, payload) SELECT gen_random_uuid(), tenant_id, 'memory.compress_day', "
      "jsonb_build_object('user_id', id, 'channel', 'web', 'local_day', $1::text) FROM users",
      {yesterday});
  ASSERT_TRUE(eventually([&] {
    const auto r = sql(user.tenantId, "SELECT last_error FROM ops.jobs WHERE tenant_id = $1::uuid", {user.tenantId});
    return !r.empty() && !r[0]["last_error"].isNull();
  }));
  const auto j = sql(user.tenantId,
                     "SELECT status, attempts, run_at > now() + interval '1 hour' AS later, last_error FROM ops.jobs "
                     "WHERE tenant_id = $1::uuid",
                     {user.tenantId})[0];
  EXPECT_EQ(j["status"].as<std::string>(), "queued");
  EXPECT_EQ(j["attempts"].as<int>(), 0);  // deferral is not a failed attempt
  EXPECT_TRUE(j["later"].as<bool>());
  EXPECT_EQ(j["last_error"].as<std::string>(), "assistant offline");
}

TEST_F(Memory, WeekRollupCombinesDaySummaries) {
  start(true);
  for (int d : {-9, -8}) {
    sql(user.tenantId,
        "INSERT INTO episodic_summaries (id, tenant_id, user_id, channel, period, period_start, content, model, "
        "prompt_version) SELECT $1::uuid, tenant_id, id, 'web', 'day', $2::date, $3, 'm', 'v' FROM users",
        {ids::newUuidV7(), day(d), "Day " + std::to_string(d) + ": gym and reading."});
  }
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"submit_summary","arguments":{"summary":"A steady week: gym and reading."}}]}])"));
  sql(user.tenantId,
      "INSERT INTO ops.jobs (id, tenant_id, kind, payload) SELECT gen_random_uuid(), tenant_id, 'memory.rollup', "
      "jsonb_build_object('user_id', id, 'channel', 'web', 'period', 'week', 'period_start', $1::text) FROM users",
      {day(-9)});
  ASSERT_TRUE(eventually([&] {
    return sql(user.tenantId, "SELECT count(*) AS n FROM episodic_summaries WHERE period = 'week'")[0]["n"].as<int>() == 1;
  }));
  EXPECT_NE(fake->requests()[0]["messages"][1]["content"].asString().find("Day -8: gym"), std::string::npos);
}
