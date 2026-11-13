// Scoring end to end (scoring_lld.md §12): a real day of plan, evidence and targets through scores.close_day with a
// scripted adjustment; idempotent and late recomputes, disputes, manual mode, unobserved and leave days, settings,
// today's progress and period scores.
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

class Scoring : public ::testing::Test {
 protected:
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  TestUser user;
  std::string yesterday;

  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    fake = std::make_unique<FakeLlm>();
    user = createTenantWithUser();
    server = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake->baseUrl()},
                                {"THELIFE_JOBS", "on"},
                                {"THELIFE_JOBS_TENANT", user.tenantId},
                                {"THELIFE_JOBS_HOUSEKEEPING", "off"},
                                {"THELIFE_JOBS_POLL_MS", "100"}});
    api = std::make_unique<ApiClient>(*server, user);
    yesterday = local(-1, 0, 0).substr(0, 10);
  }

  // The user's local day (+offset days), at hh:mm, as RFC 3339 with the user's offset (test users are UTC).
  std::string local(int dayOffset, int hh, int mm) {
    return sql(user.tenantId,
               "SELECT to_char(((now() AT TIME ZONE timezone) - make_interval(hours => day_start_hour))::date + $1::int "
               "+ make_interval(hours => $2::int, mins => $3::int), 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS t FROM users",
               {std::to_string(dayOffset), std::to_string(hh), std::to_string(mm)})[0]["t"].as<std::string>();
  }

  void runDay(const std::string& day, const std::string& extra = "") {
    sql(user.tenantId,
        "INSERT INTO ops.jobs (id, tenant_id, kind, payload) SELECT gen_random_uuid(), tenant_id, 'scores.close_day', "
        "jsonb_build_object('user_id', id, 'local_day', $1::text)" + extra + " FROM users",
        {day});
  }

  int versions(const std::string& day) {
    return sql(user.tenantId, "SELECT count(*)::int AS n FROM score_versions WHERE period = 'day' AND period_start = $1::date",
               {day})[0]["n"].as<int>();
  }

  void waitIdle() {
    ASSERT_TRUE(eventually([&] {
      return sql(user.tenantId, "SELECT count(*)::int AS n FROM ops.jobs WHERE tenant_id = $1::uuid AND status IN ('queued', 'running') "
                                "AND run_at <= now()",
                 {user.tenantId})[0]["n"].as<int>() == 0;
    }));
  }

  // A planned day: routine done on time (personal), one-off work task not done, a 07:00-08:00 fitness block with
  // 30 min of exercise and 30 min of gaming, and a 10-page reading target with 15 pages read.
  void planYesterday() {
    const auto routine = api->post("/tasks", J(R"({"title":"Journal","area":"personal","rrule":"FREQ=DAILY",
        "rrule_dtstart":"2026-01-01T00:00:00+00:00"})"));
    ASSERT_EQ(routine.status, 201);
    Json::Value done;
    done["completed_at"] = local(-1, 12, 0);
    ASSERT_EQ(api->post("/tasks/" + routine.json["id"].asString() + "/occurrences/" + yesterday + ":complete", done).status, 200);
    Json::Value report = J(R"({"title":"Send report","area":"work"})");
    report["due_at"] = local(-1, 17, 0);
    ASSERT_EQ(api->post("/tasks", report).status, 201);
    Json::Value gym = J(R"({"title":"Gym","area":"fitness"})");
    gym["starts_at"] = local(-1, 7, 0), gym["ends_at"] = local(-1, 8, 0);
    ASSERT_EQ(api->post("/events", gym).status, 201);
    Json::Value ex = J(R"({"title":"Treadmill","area":"fitness"})");
    ex["started_at"] = local(-1, 7, 0), ex["ended_at"] = local(-1, 7, 30);
    ASSERT_EQ(api->post("/activities", ex).status, 201);
    Json::Value game = J(R"({"title":"Gaming","area":"entertainment"})");
    game["started_at"] = local(-1, 7, 30), game["ended_at"] = local(-1, 8, 0);
    ASSERT_EQ(api->post("/activities", game).status, 201);
    ASSERT_EQ(api->post("/targets", J(R"({"title":"Read 10 pages","area":"skill","metric":"pages","amount":10})")).status, 201);
    const auto book = api->post("/skills/resources", J(R"({"kind":"book","title":"SICP","total_pages":600})"));
    Json::Value s = J(R"({"page_from":1,"page_to":16})");
    s["resource_id"] = book.json["id"];
    s["started_at"] = local(-1, 21, 0), s["ended_at"] = local(-1, 21, 40);
    ASSERT_EQ(api->post("/skills/sessions", s).status, 201);
  }
};

}  // namespace

TEST_F(Scoring, DayScoreWithAdjustmentRecomputeAndDispute) {
  planYesterday();
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"submit_score_adjustment","arguments":{
      "adjustment_pct":5,"reasons":[{"text":"Strong reading progress on an active skill goal.","direction":"up"}],
      "goal_alignment":"medium"}}]}])"));
  runDay(yesterday);
  ASSERT_TRUE(eventually([&] { return versions(yesterday) == 1; }));

  const auto list = api->get("/scores?period=day").json["items"];
  ASSERT_EQ(list.size(), 1u);
  const auto s = list[0];
  EXPECT_EQ(s["status"].asString(), "final");
  EXPECT_EQ(s["profile"].asString(), "workday");
  const auto& a = s["breakdown"]["areas"];
  EXPECT_DOUBLE_EQ(a["personal"]["score"].asDouble(), 100);  // routine done on time
  EXPECT_DOUBLE_EQ(a["work"]["score"].asDouble(), 0);        // report not sent
  EXPECT_DOUBLE_EQ(a["fitness"]["score"].asDouble(), 50);    // 30 of 60 block minutes
  EXPECT_DOUBLE_EQ(a["skill"]["score"].asDouble(), 100);     // 15/10 pages: capped, +5 bonus
  // (0.30·0 + 0.25·100 + 0.15·50 + 0.15·100) / 0.85
  EXPECT_DOUBLE_EQ(s["base"].asDouble(), 55.88);
  EXPECT_DOUBLE_EQ(s["adjustment_pct"].asDouble(), 5);
  EXPECT_DOUBLE_EQ(s["final"].asDouble(), 57);  // round(55.88 + 0.3 · 5)
  EXPECT_EQ(s["adjustment"]["reasons"][0]["direction"].asString(), "up");

  // What the adjustment saw.
  const auto req = fake->requests()[0];
  const auto input = J(req["messages"][1]["content"].asString());
  EXPECT_DOUBLE_EQ(input["base"].asDouble(), 55.88);
  EXPECT_EQ(input["preemptions"][0]["kind"].asString(), "overlap");
  EXPECT_EQ(req["reasoning_effort"].asString(), "low");

  // The gaming overlapped the gym block, and ran while the report was due and never sent.
  const auto pre = api->get("/insights/preemptions?from=" + yesterday + "&to=" + yesterday).json["items"];
  ASSERT_EQ(pre.size(), 2u);
  EXPECT_EQ(pre[0]["kind"].asString(), "overlap");
  EXPECT_EQ(pre[0]["displaced_title"].asString(), "Gym");
  EXPECT_EQ(pre[0]["minutes"].asInt(), 30);
  EXPECT_EQ(pre[1]["kind"].asString(), "while_due");
  EXPECT_EQ(pre[1]["displaced_title"].asString(), "Send report");

  // Same inputs: no new version.
  runDay(yesterday);
  waitIdle();
  EXPECT_EQ(versions(yesterday), 1);

  // A late log changes the inputs; the base barely moves, so the adjustment is reused without a model call.
  Json::Value late = J(R"({"title":"Code review","area":"work"})");
  late["started_at"] = local(-1, 14, 0), late["ended_at"] = local(-1, 16, 0);
  ASSERT_EQ(api->post("/activities", late).status, 201);
  runDay(yesterday);
  ASSERT_TRUE(eventually([&] { return versions(yesterday) == 2; }));
  EXPECT_EQ(fake->requests().size(), 1u);
  const auto current = api->get("/scores?period=day").json["items"][0];
  EXPECT_DOUBLE_EQ(current["adjustment_pct"].asDouble(), 5);
  EXPECT_EQ(api->get("/scores/" + current["id"].asString() + "/history").json["items"].size(), 2u);

  // The user disputes the adjustment: one re-run with the reason.
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"submit_score_adjustment","arguments":{
      "adjustment_pct":0,"reasons":[],"goal_alignment":"medium"}}]}])"));
  const auto d = api->post("/scores/" + current["id"].asString() + ":dispute", J(R"({"reason":"Reading was planned anyway"})"));
  ASSERT_EQ(d.status, 202) << d.body;
  EXPECT_EQ(api->post("/scores/" + current["id"].asString() + ":dispute", J(R"({"reason":"again"})")).status, 409);
  ASSERT_TRUE(eventually([&] { return versions(yesterday) == 3; }));
  const auto after = api->get("/scores?period=day").json["items"][0];
  EXPECT_DOUBLE_EQ(after["adjustment_pct"].asDouble(), 0);
  EXPECT_DOUBLE_EQ(after["final"].asDouble(), 56);
  EXPECT_NE(fake->requests()[1]["messages"][1]["content"].asString().find("Reading was planned anyway"), std::string::npos);
  const auto disputeRow = sql(user.tenantId, "SELECT status, before_pct::float8 AS b, after_pct::float8 AS a FROM score_disputes")[0];
  EXPECT_EQ(disputeRow["status"].as<std::string>(), "done");
  EXPECT_DOUBLE_EQ(disputeRow["b"].as<double>(), 5);
  EXPECT_DOUBLE_EQ(disputeRow["a"].as<double>(), 0);
}

TEST_F(Scoring, ManualModeStoresAProvisionalBaseAndAdjustsAfterTheReset) {
  planYesterday();
  fake->script(J(R"([{"error":{"status":429,"body":{"error":"weekly usage limit reached"}}}])"));
  runDay(yesterday);
  ASSERT_TRUE(eventually([&] { return versions(yesterday) == 1; }));
  const auto s = api->get("/scores?period=day").json["items"][0];
  EXPECT_EQ(s["status"].asString(), "provisional");
  EXPECT_DOUBLE_EQ(s["base"].asDouble(), 55.88);
  EXPECT_TRUE(s["final"].isNull());
  const auto later = sql(user.tenantId,
                         "SELECT payload->>'force_adjust' AS f, run_at > now() + interval '1 hour' AS later FROM ops.jobs "
                         "WHERE tenant_id = $1::uuid AND dedupe_key LIKE 'adjust:%' AND status = 'queued'",
                         {user.tenantId});
  ASSERT_EQ(later.size(), 1u);
  EXPECT_EQ(later[0]["f"].as<std::string>(), "true");
  EXPECT_TRUE(later[0]["later"].as<bool>());
}

TEST_F(Scoring, UnobservedAndLeaveDaysAreNotScored) {
  Json::Value t = J(R"({"title":"Taxes","area":"personal"})");
  t["due_at"] = local(-1, 17, 0);
  ASSERT_EQ(api->post("/tasks", t).status, 201);
  fake->script(J("[]"));
  runDay(yesterday);
  ASSERT_TRUE(eventually([&] { return versions(yesterday) == 1; }));
  auto s = api->get("/scores?period=day").json["items"][0];
  EXPECT_EQ(s["status"].asString(), "unobserved");
  EXPECT_TRUE(s["base"].isNull());

  Json::Value o;
  o["local_day"] = yesterday;
  o["profile"] = "leave";
  ASSERT_EQ(api->post("/day-profile-overrides", o).status, 201);
  runDay(yesterday);
  ASSERT_TRUE(eventually([&] { return versions(yesterday) == 2; }));
  s = api->get("/scores?period=day").json["items"][0];
  EXPECT_EQ(s["status"].asString(), "not_scored");
  EXPECT_EQ(s["profile"].asString(), "leave");
  EXPECT_TRUE(fake->requests().empty());  // no model call for days without a base
}

TEST_F(Scoring, SettingsAndTodaysProgress) {
  auto s = api->get("/settings/scoring");
  ASSERT_EQ(s.status, 200);
  EXPECT_DOUBLE_EQ(s.json["weights"]["work"].asDouble(), 0.30);
  EXPECT_DOUBLE_EQ(s.json["plan_share"]["skill"].asDouble(), 0.8);
  const auto p = api->call("PATCH", "/settings/scoring", J(R"({"weights":{"work":0.5},"plan_share":{"fitness":0.6}})"));
  ASSERT_EQ(p.status, 200) << p.body;
  EXPECT_DOUBLE_EQ(api->get("/settings/scoring").json["weights"]["work"].asDouble(), 0.5);
  EXPECT_DOUBLE_EQ(api->get("/settings/scoring").json["plan_share"]["fitness"].asDouble(), 0.6);
  EXPECT_EQ(api->call("PATCH", "/settings/scoring",
                      J(R"({"weights":{"work":0,"skill":0,"fitness":0,"personal":0,"consistency":0}})")).status,
            422);
  EXPECT_EQ(api->call("PATCH", "/settings/scoring", J("{}")).status, 422);  // found by schemathesis

  const auto now = sql(user.tenantId, "SELECT to_char(now() - interval '1 hour', 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS a, "
                                      "to_char(now() + interval '1 hour', 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS b, "
                                      "to_char(now() - interval '30 minutes', 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS c, "
                                      "to_char(now(), 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS d")[0];
  Json::Value block = J(R"({"title":"Deep work","area":"work"})");
  block["starts_at"] = now["a"].as<std::string>(), block["ends_at"] = now["b"].as<std::string>();
  ASSERT_EQ(api->post("/events", block).status, 201);
  Json::Value act = J(R"({"title":"Coding","area":"work"})");
  act["started_at"] = now["c"].as<std::string>(), act["ended_at"] = now["d"].as<std::string>();
  ASSERT_EQ(api->post("/activities", act).status, 201);
  const auto prog = api->get("/progress/today");
  ASSERT_EQ(prog.status, 200) << prog.body;
  const auto& w = prog.json["areas"]["work"];
  EXPECT_NEAR(w["block_minutes_so_far"].asInt(), 60, 1);
  EXPECT_NEAR(w["block_minutes_covered"].asInt(), 30, 1);
  EXPECT_FALSE(prog.json.isMember("score"));
}

TEST_F(Scoring, WeekScoreIsTheMeanOfScoredDaysWithCoverage) {
  const auto monday = sql(user.tenantId, "SELECT to_char(date_trunc('week', current_date - 7), 'YYYY-MM-DD') AS d")[0]["d"].as<std::string>();
  int k = 0;
  for (const auto& [status, final] : std::vector<std::pair<std::string, std::string>>{{"final", "80"}, {"final", "60"},
                                                                                     {"unobserved", ""}, {"not_scored", ""}}) {
    sql(user.tenantId,
        "INSERT INTO score_versions (id, tenant_id, user_id, period, period_start, formula_version, inputs_hash, status, "
        "base, final) SELECT gen_random_uuid(), tenant_id, id, 'day', $1::date + " + std::to_string(k++) +
            ", '1', 'h', $2, nullif($3, '')::numeric, nullif($3, '')::numeric FROM users",
        {monday, status, final});
  }
  sql(user.tenantId,
      "INSERT INTO ops.jobs (id, tenant_id, kind, payload) SELECT gen_random_uuid(), tenant_id, 'scores.close_period', "
      "jsonb_build_object('user_id', id, 'period', 'week', 'period_start', $1::text) FROM users",
      {monday});
  ASSERT_TRUE(eventually([&] { return api->get("/scores?period=week").json["items"].size() == 1; }));
  const auto w = api->get("/scores?period=week").json["items"][0];
  EXPECT_DOUBLE_EQ(w["final"].asDouble(), 70);
  EXPECT_EQ(w["coverage"]["scored_days"].asInt(), 2);
  EXPECT_EQ(w["coverage"]["days"].asInt(), 7);
}
