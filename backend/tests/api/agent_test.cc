// Agent turns end to end (agent_lld.md §4–7, §12): the real binary against docker/fake-llm. Covers chat capture
// provenance, tool groups, approvals (pinned version, stale, reject, expiry), kill switch, named policies, manual
// mode and commands, limits, idempotent sends, the SSE stream, turn merging and desktop notification delivery.
#include <curl/curl.h>
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

Json::Value J(const char* text) { return json::parse(text); }

// Runs SQL in the tenant's RLS context as the app role.
// Runs and commits before returning (Drogon commits asynchronously when the transaction is released).
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

class Agent : public ::testing::Test {
 protected:
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  TestUser user;

  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    fake = std::make_unique<FakeLlm>();
    server = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake->baseUrl()}});
    user = createTenantWithUser();
    api = std::make_unique<ApiClient>(*server, user);
  }

  Json::Value send(const std::string& text, const std::string& clientId = {}) {
    Json::Value b;
    b["text"] = text;
    if (!clientId.empty()) b["client_msg_id"] = clientId;
    const auto r = api->post("/chat/web/messages", b);
    EXPECT_EQ(r.status, 202) << r.body;
    return r.json;
  }

  Json::Value waitTurn(const std::string& id) {
    for (int i = 0; i < 200; ++i) {
      const auto r = api->get("/chat/turns/" + id);
      const auto s = r.json["status"].asString();
      if (s != "queued" && s != "running") return r.json;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ADD_FAILURE() << "turn " << id << " did not finish";
    return {};
  }

  Json::Value chat(const std::string& text) { return waitTurn(send(text)["id"].asString()); }

  // The final assistant message of one turn.
  std::string reply(const Json::Value& turn) {
    std::string out;
    for (const auto& m : messages()) {
      if (m["turn_id"].asString() == turn["id"].asString() && m["role"].asString() == "assistant") {
        out = m["content"].asString();
      }
    }
    return out;
  }
  std::string say(const std::string& text) { return reply(chat(text)); }

  Json::Value messages() {
    const auto convs = api->get("/conversations?channel=web").json["items"];
    if (convs.empty()) return Json::Value(Json::arrayValue);
    return api->get("/conversations/" + convs[0]["id"].asString() + "/messages").json["items"];
  }

  std::string lastReply() {
    const auto m = messages();
    for (int i = static_cast<int>(m.size()) - 1; i >= 0; --i) {
      if (m[i]["role"].asString() == "assistant" && !m[i]["content"].asString().empty()) return m[i]["content"].asString();
    }
    return "";
  }

  // Waits until every approval-triggered turn of the tenant has finished.
  void waitApprovalTurns(int expected) {
    for (int i = 0; i < 200; ++i) {
      const auto r = sql(user.tenantId,
                         "SELECT count(*) FILTER (WHERE status NOT IN ('queued','running')) AS done FROM turns "
                         "WHERE trigger = 'approval'");
      if (r[0]["done"].as<int>() >= expected) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ADD_FAILURE() << "approval turns did not finish";
  }

  Json::Value userTask(const char* title, const char* extra = "") {
    auto body = J((std::string(R"({"title":")") + title + R"(","area":"personal")" + extra + "}").c_str());
    const auto r = api->post("/tasks", body);
    EXPECT_EQ(r.status, 201) << r.body;
    return r.json;
  }
};

}  // namespace

// ------------------------------------------------------------------------------------------------ capture

TEST_F(Agent, ChatCaptureCreatesUserStatedItemsWithAuditAndReceipt) {
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"tasks_create","arguments":{"title":"Renew passport","area":"personal","priority":2}},
                                {"name":"activities_create","arguments":{"title":"Gym","area":"fitness",
                                 "started_at":"2026-11-13T07:00:00+05:30","ended_at":"2026-11-13T07:40:00+05:30"}}]},
    {"content":"Logged: Gym 40 min (fitness). Task: Renew passport."}])"));
  const auto turn = chat("Did 40 min gym this morning, and I need to renew my passport");
  EXPECT_EQ(turn["status"].asString(), "done");
  EXPECT_EQ(lastReply(), "Logged: Gym 40 min (fitness). Task: Renew passport.");

  const auto userMsg = turn["message_id"].asString();
  const auto task = api->get("/tasks").json["items"][0];
  EXPECT_EQ(task["title"].asString(), "Renew passport");
  EXPECT_EQ(task["origin"].asString(), "user_stated");
  EXPECT_EQ(task["created_by"]["kind"].asString(), "agent");
  EXPECT_EQ(task["source_message_id"].asString(), userMsg);
  EXPECT_EQ(api->get("/activities").json["items"][0]["origin"].asString(), "user_stated");

  const auto audit = sql(user.tenantId, "SELECT tool, decision, outcome FROM agent_actions ORDER BY created_at, id");
  ASSERT_EQ(audit.size(), 2u);
  EXPECT_EQ(audit[0]["tool"].as<std::string>(), "tasks.create");
  EXPECT_EQ(audit[0]["decision"].as<std::string>(), "allow");
  EXPECT_EQ(audit[0]["outcome"].as<std::string>(), "executed");

  // Transcript: user, assistant(tool calls), tool, tool, assistant.
  const auto m = messages();
  ASSERT_EQ(m.size(), 5u);
  EXPECT_EQ(m[1]["tool_calls"].size(), 2u);
  EXPECT_EQ(m[2]["role"].asString(), "tool");
  EXPECT_EQ(m[2]["tool_call_id"].asString(), m[1]["tool_calls"][0]["id"].asString());

  // What the model saw: rendered system prompt, the Now note, core tools only, the user's message last.
  const auto req = fake->requests()[0];
  const auto& msgs = req["messages"];
  EXPECT_NE(msgs[0]["content"].asString().find("personal planning assistant of"), std::string::npos);
  EXPECT_EQ(msgs[0]["content"].asString().find("{{"), std::string::npos);
  EXPECT_NE(msgs[1]["content"].asString().find("Now: "), std::string::npos);
  EXPECT_EQ(msgs[msgs.size() - 1]["role"].asString(), "user");
  std::set<std::string> tools;
  for (const auto& t : req["tools"]) tools.insert(t["function"]["name"].asString());
  EXPECT_TRUE(tools.count("tasks_create"));
  EXPECT_TRUE(tools.count("context_now"));
  EXPECT_TRUE(tools.count("agent_load_tools"));
  EXPECT_FALSE(tools.count("goals_create"));
  // Budget for the always-sent core group (docs/tool_registry.md §6): about 3.7k tokens today.
  EXPECT_LT(json::compact(req["tools"]).size(), 18000u);
  EXPECT_TRUE(req["stream"].asBool());

  const auto usage = sql(user.tenantId, "SELECT sum(requests)::int AS n FROM llm_usage WHERE purpose = 'agent.turn'");
  EXPECT_EQ(usage[0]["n"].as<int>(), 2);
}

TEST_F(Agent, ToolGroupsLoadOnDemandAndUnloadedToolsAreRejected) {
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"goals_create","arguments":{"title":"Learn Rust","area":"skill","horizon":"quarter"}}]},
    {"content":"","tool_calls":[{"name":"agent_load_tools","arguments":{"group":"planning"}}]},
    {"content":"","tool_calls":[{"name":"goals_create","arguments":{"title":"Learn Rust","area":"skill","horizon":"quarter"}}]},
    {"content":"Goal added."}])"));
  EXPECT_EQ(chat("new goal: learn rust this quarter")["status"].asString(), "done");
  const auto goals = api->get("/goals").json["items"];
  ASSERT_EQ(goals.size(), 1u);  // the first, unloaded call did nothing
  const auto reqs = fake->requests();
  auto has = [](const Json::Value& req, const std::string& name) {
    for (const auto& t : req["tools"]) {
      if (t["function"]["name"].asString() == name) return true;
    }
    return false;
  };
  EXPECT_FALSE(has(reqs[1], "goals_create"));
  EXPECT_TRUE(has(reqs[2], "goals_create"));
  const auto m = messages();
  EXPECT_NE(m[2]["content"].asString().find("agent_load_tools"), std::string::npos);  // error told it how to fix
  const auto audit = sql(user.tenantId, "SELECT decision FROM agent_actions ORDER BY created_at, id");
  EXPECT_EQ(audit[0]["decision"].as<std::string>(), "invalid");
}

// ------------------------------------------------------------------------------------------------ approvals

TEST_F(Agent, EditingAUserItemWaitsForApprovalThenRunsPinnedToVersion) {
  const auto task = userTask("Gym 3x per week");
  const auto id = task["id"].asString();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"tasks_update","arguments":{"id":")" + id + R"(","title":"Gym 4x per week"}}]},
    {"content":"That change needs your approval."},
    {"content":"Done: Gym is now 4x per week."}])").c_str()));
  EXPECT_EQ(chat("make gym 4 times a week")["status"].asString(), "done");
  EXPECT_EQ(api->get("/tasks/" + id).json["title"].asString(), "Gym 3x per week");  // nothing ran yet

  const auto pending = api->get("/approvals?status=pending").json["items"];
  ASSERT_EQ(pending.size(), 1u);
  const auto& a = pending[0];
  EXPECT_EQ(a["tool"].asString(), "tasks.update");
  EXPECT_EQ(a["target_id"].asString(), id);
  EXPECT_EQ(a["target_version"].asInt(), 1);
  EXPECT_EQ(a["created_by"]["kind"].asString(), "agent");
  EXPECT_NE(a["summary"].asString().find("Gym 4x"), std::string::npos);
  // The model was told it is pending.
  EXPECT_NE(messages()[2]["content"].asString().find("pending_approval"), std::string::npos);

  const auto approved = api->post("/approvals/" + a["id"].asString() + ":approve", Json::Value(Json::objectValue));
  ASSERT_EQ(approved.status, 200) << approved.body;
  EXPECT_EQ(approved.json["status"].asString(), "approved");
  EXPECT_FALSE(approved.json["decided_at"].isNull());
  EXPECT_EQ(api->get("/tasks/" + id).json["title"].asString(), "Gym 4x per week");
  EXPECT_EQ(api->post("/approvals/" + a["id"].asString() + ":approve", Json::Value(Json::objectValue)).status, 409);

  // The agent hears about the decision and tells the user (approval trigger, §4.1).
  waitApprovalTurns(1);
  EXPECT_EQ(lastReply(), "Done: Gym is now 4x per week.");
  const auto audit = sql(user.tenantId, "SELECT decision, outcome FROM agent_actions ORDER BY created_at, id");
  ASSERT_EQ(audit.size(), 2u);
  EXPECT_EQ(audit[0]["decision"].as<std::string>(), "approval");
  EXPECT_EQ(audit[0]["outcome"].as<std::string>(), "pending");
  EXPECT_EQ(audit[1]["decision"].as<std::string>(), "approved");
  EXPECT_EQ(audit[1]["outcome"].as<std::string>(), "executed");
}

TEST_F(Agent, ApprovalOfAChangedTargetIsStaleAndRunsNothing) {
  const auto id = userTask("Read 20 pages")["id"].asString();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"tasks_update","arguments":{"id":")" + id + R"(","title":"Read 30 pages"}}]},
    {"content":"Needs approval."},
    {"content":"It changed meanwhile; nothing done."}])").c_str()));
  chat("raise reading to 30 pages");
  const auto approvalId = api->get("/approvals?status=pending").json["items"][0]["id"].asString();
  ASSERT_EQ(api->patch("/tasks/" + id, J(R"({"title":"Read 25 pages"})"), "\"1\"").status, 200);  // user edits

  const auto r = api->post("/approvals/" + approvalId + ":approve", Json::Value(Json::objectValue));
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["status"].asString(), "stale");
  EXPECT_EQ(api->get("/tasks/" + id).json["title"].asString(), "Read 25 pages");
  waitApprovalTurns(1);
  EXPECT_NE(fake->requests()[2]["messages"][fake->requests()[2]["messages"].size() - 1]["content"].asString().find("stale"),
            std::string::npos);
}

TEST_F(Agent, RejectedAndExpiredApprovalsRunNothing) {
  const auto id = userTask("Call bank")["id"].asString();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"agent_load_tools","arguments":{"group":"planning"}}]},
    {"content":"","tool_calls":[{"name":"tasks_delete","arguments":{"id":")" + id + R"("}}]},
    {"content":"Waiting for approval."},
    {"content":"Okay, keeping it."}])").c_str()));
  chat("drop the bank task");
  const auto approvalId = api->get("/approvals?status=pending").json["items"][0]["id"].asString();
  const auto r = api->post("/approvals/" + approvalId + ":reject", J(R"({"reason":"still need it"})"));
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["status"].asString(), "rejected");
  EXPECT_EQ(r.json["decision_reason"].asString(), "still need it");
  EXPECT_EQ(api->get("/tasks/" + id).status, 200);
  EXPECT_EQ(api->post("/approvals/" + approvalId + ":approve", Json::Value(Json::objectValue)).status, 409);
  waitApprovalTurns(1);

  // An approval past its 72 h reads as expired and cannot be approved.
  const auto expiredId = ids::newUuidV7();
  sql(user.tenantId,
      "INSERT INTO approval_requests (id, tenant_id, created_by_kind, user_id, tool, args, summary, expires_at, "
      "created_at) SELECT $1::uuid, tenant_id, 'agent', id, 'tasks.delete', jsonb_build_object('id', $2::text), "
      "'old', now() - interval '1 hour', now() - interval '73 hours' FROM users",
      {expiredId, id});
  EXPECT_EQ(api->get("/approvals/" + expiredId).json["status"].asString(), "expired");
  EXPECT_EQ(api->get("/approvals?status=expired").json["items"].size(), 1u);
  EXPECT_EQ(api->post("/approvals/" + expiredId + ":approve", Json::Value(Json::objectValue)).status, 409);
  EXPECT_EQ(api->get("/tasks/" + id).status, 200);
}

TEST_F(Agent, KillSwitchDeniesAgentWritesButNotReads) {
  adminDb()->execSqlSync("SELECT admin.set_agent_writes($1::uuid, false)", user.tenantId);
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"tasks_list","arguments":{}},{"name":"tasks_create","arguments":{"title":"X","area":"other"}}]},
    {"content":"Writes are disabled."}])"));
  EXPECT_EQ(chat("add task X")["status"].asString(), "done");
  EXPECT_EQ(api->get("/tasks").json["items"].size(), 0u);
  const auto audit = sql(user.tenantId, "SELECT tool, decision, outcome FROM agent_actions ORDER BY created_at, id");
  ASSERT_EQ(audit.size(), 2u);
  EXPECT_EQ(audit[0]["outcome"].as<std::string>(), "executed");  // read
  EXPECT_EQ(audit[1]["decision"].as<std::string>(), "deny");
  EXPECT_NE(fake->requests()[0]["messages"][1]["content"].asString().find("disabled"), std::string::npos);
}

TEST_F(Agent, RoutineProgressReportedByTheUserAndUndoOfOwnItemsNeedNoApproval) {
  const auto routine = userTask("Read", R"(,"rrule":"FREQ=DAILY")");
  const auto rid = routine["id"].asString();
  const auto today = sql(user.tenantId,
                         "SELECT to_char(((now() AT TIME ZONE timezone) - make_interval(hours => day_start_hour))::date,"
                         " 'YYYY-MM-DD') AS d FROM users")[0]["d"].as<std::string>();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"tasks_complete_occurrence","arguments":{"id":")" + rid + R"(","date":")" +
                  today + R"("}},{"name":"tasks_create","arguments":{"title":"Buy milk","area":"personal"}}]},
    {"content":"Logged reading. Added Buy milk."}])").c_str()));
  chat("done with reading today, also buy milk");
  const auto occ = api->get("/tasks/" + rid + "/occurrences?from=" + today + "&to=" + today).json["items"];
  ASSERT_EQ(occ.size(), 1u);
  EXPECT_EQ(occ[0]["status"].asString(), "done");
  const auto milk = api->get("/tasks?recurring=false").json["items"][0]["id"].asString();

  // "undo 2": the agent removes what it created minutes ago, without approval.
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"agent_load_tools","arguments":{"group":"planning"}}]},
    {"content":"","tool_calls":[{"name":"tasks_delete","arguments":{"id":")" + milk + R"("}}]},
    {"content":"Removed Buy milk."}])").c_str()));
  chat("undo 2");
  EXPECT_EQ(api->get("/tasks/" + milk).status, 404);
  EXPECT_EQ(api->get("/approvals").json["items"].size(), 0u);
}

TEST_F(Agent, ApprovalTriggeredTurnsDoNotGetUserProgressPolicy) {
  // The named policy needs a user message: on a proactive/approval turn the same call is held.
  const auto rid = userTask("Stretch", R"(,"rrule":"FREQ=DAILY")")["id"].asString();
  const auto id = userTask("Other")["id"].asString();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"tasks_update","arguments":{"id":")" + id + R"(","title":"Other 2"}}]},
    {"content":"Needs approval."},
    {"content":"","tool_calls":[{"name":"tasks_complete_occurrence","arguments":{"id":")" + rid +
                  R"(","date":"2026-01-05"}}]},
    {"content":"Asked."}])").c_str()));
  chat("rename other");
  const auto approvalId = api->get("/approvals?status=pending").json["items"][0]["id"].asString();
  api->post("/approvals/" + approvalId + ":reject", Json::Value(Json::objectValue));
  waitApprovalTurns(1);
  EXPECT_EQ(api->get("/approvals?status=pending").json["items"].size(), 1u);
  EXPECT_EQ(api->get("/approvals?status=pending").json["items"][0]["tool"].asString(), "tasks.complete_occurrence");
}

// ------------------------------------------------------------------------------------------------ manual mode

TEST_F(Agent, WeeklyLimitSwitchesChatToDeterministicModeAndCommandsStillWork) {
  userTask("Water plants", R"(,"rrule":"FREQ=DAILY")");
  fake->script(J(R"([{"error":{"status":429,"body":{"error":"you've reached your weekly usage limit"}}}])"));
  const auto t = chat("how is my day?");
  EXPECT_EQ(t["status"].asString(), "manual");
  const auto offline = reply(t);
  EXPECT_NE(offline.find("offline until"), std::string::npos) << offline;
  EXPECT_NE(offline.find("/today"), std::string::npos);

  const auto status = api->get("/system/llm-status").json;
  EXPECT_EQ(status["mode"].asString(), "manual");
  EXPECT_FALSE(status["resets_at"].isNull());

  // No model call is made from here on; commands answer.
  const auto before = fake->requests().size();
  EXPECT_EQ(chat("anything new?")["status"].asString(), "manual");
  const auto today = say("/today");
  EXPECT_NE(today.find("1. (all day) Water plants [todo]"), std::string::npos) << today;
  EXPECT_EQ(say("/done 1"), "Done: Water plants");
  EXPECT_NE(say("/today").find("Water plants [done]"), std::string::npos);
  EXPECT_NE(say("/frobnicate").find("Unknown command"), std::string::npos);
  EXPECT_NE(say("/done 9").find("no item 9"), std::string::npos);
  EXPECT_EQ(fake->requests().size(), before);
}

TEST_F(Agent, ApprovalsCanBeDecidedByCommand) {
  const auto id = userTask("Laundry")["id"].asString();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"tasks_update","arguments":{"id":")" + id + R"(","priority":3}}]},
    {"content":"Needs approval."},
    {"content":"Raised."}])").c_str()));
  chat("laundry is urgent");
  EXPECT_NE(say("/approvals").find("1. "), std::string::npos);
  EXPECT_EQ(say("/approve 1"), "Approved and done.");
  EXPECT_EQ(api->get("/tasks/" + id).json["priority"].asInt(), 3);
  EXPECT_NE(say("/approve 1").find("no pending approval 1"), std::string::npos);
}

TEST(AgentUnconfigured, RepliesWithTheCommandHint) {
  THELIFE_SKIP_WITHOUT_DB();
  auto server = startServerWithDb();
  ApiClient api(*server, createTenantWithUser());
  Json::Value b;
  b["text"] = "hello";
  const auto t = api.post("/chat/web/messages", b).json;
  Json::Value turn;
  for (int i = 0; i < 100; ++i) {
    turn = api.get("/chat/turns/" + t["id"].asString()).json;
    if (turn["status"].asString() != "queued" && turn["status"].asString() != "running") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_EQ(turn["status"].asString(), "manual");
  const auto conv = api.get("/conversations").json["items"][0]["id"].asString();
  const auto msgs = api.get("/conversations/" + conv + "/messages").json["items"];
  EXPECT_NE(msgs[1]["content"].asString().find("isn't set up"), std::string::npos);
  EXPECT_EQ(msgs[1]["kind"].asString(), "deterministic");
}

// ------------------------------------------------------------------------------------------------ limits, failures

TEST_F(Agent, StepLimitEndsTheTurnWithASummary) {
  Json::Value script(Json::arrayValue);
  for (int i = 0; i < 8; ++i) script.append(J(R"({"content":"","tool_calls":[{"name":"context_now","arguments":{}}]})"));
  fake->script(script);
  const auto t = chat("loop forever");
  EXPECT_EQ(t["status"].asString(), "done");
  EXPECT_NE(lastReply().find("stopped after 8 steps"), std::string::npos);
  EXPECT_EQ(fake->requests().size(), 8u);
  // context_now really answered with today's plan.
  EXPECT_NE(messages()[2]["content"].asString().find("\"local_day\""), std::string::npos);
}

TEST_F(Agent, ModelErrorsFailTheTurnPolitelyAndBadArgumentsAreReportedToTheModel) {
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"tasks_create","arguments":"{not json"},{"name":"tasks_create","arguments":{"area":"nap"}}]},
    {"error":{"status":400,"body":{"error":{"message":"bad request","type":"invalid_request_error"}}}}])"));
  const auto t = chat("add something");
  EXPECT_EQ(t["status"].asString(), "failed");
  EXPECT_EQ(t["error"].asString(), "bad_request");
  const auto m = messages();
  EXPECT_NE(m[2]["content"].asString().find("JSON object"), std::string::npos);
  EXPECT_NE(m[3]["content"].asString().find("/title"), std::string::npos);  // schema errors with pointers
  EXPECT_EQ(m[4]["kind"].asString(), "error");
  EXPECT_NE(m[4]["content"].asString().find("Your message is saved"), std::string::npos);
}

// ------------------------------------------------------------------------------------------------ the API surface

TEST_F(Agent, SendValidatesAndIsIdempotent) {
  const auto a = send("hi there", "client-msg-0001");
  const auto b = send("hi there", "client-msg-0001");
  EXPECT_EQ(a["id"].asString(), b["id"].asString());
  waitTurn(a["id"].asString());
  EXPECT_EQ(fake->requests().size(), 1u);

  EXPECT_EQ(api->post("/chat/web/messages", J(R"({"text":""})")).status, 422);
  EXPECT_EQ(api->post("/chat/web/messages", J(R"({"text":"x","extra":1})")).status, 422);
  EXPECT_EQ(api->post("/chat/web/messages", J(R"({"text":"x","client_msg_id":"short"})")).status, 422);
  EXPECT_EQ(api->get("/chat/turns/" + ids::newUuidV7()).status, 404);
  EXPECT_EQ(api->get("/chat/turns/nope").status, 422);
  EXPECT_EQ(sendJson("POST", server->baseUrl() + "/api/v1/chat/web/messages", J(R"({"text":"x"})")).status, 401);

  // Another tenant sees none of it.
  ApiClient other(*server, createTenantWithUser());
  EXPECT_EQ(other.get("/chat/turns/" + a["id"].asString()).status, 404);
  EXPECT_EQ(other.get("/conversations").json["items"].size(), 0u);
  const auto conv = api->get("/conversations").json["items"][0]["id"].asString();
  EXPECT_EQ(other.get("/conversations/" + conv + "/messages").status, 404);
}

namespace {
size_t collect(char* d, size_t s, size_t n, void* out) {
  static_cast<std::string*>(out)->append(d, s * n);
  return s * n;
}
}  // namespace

TEST_F(Agent, StreamCarriesTokensToolsAndTurnEvents) {
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"context_now","arguments":{}}]},
    {"content":"Your day looks clear."}])"));
  std::string body;
  std::thread reader([&] {
    CURL* c = curl_easy_init();
    curl_slist* h = curl_slist_append(nullptr, ("Authorization: Bearer " + api->token()).c_str());
    curl_easy_setopt(c, CURLOPT_URL, (server->baseUrl() + "/api/v1/chat/web/stream").c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 2500L);
    curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const auto t = chat("how's my day");
  reader.join();
  EXPECT_NE(body.find(": connected"), std::string::npos);
  for (const char* ev : {"event: turn.started", "event: tool.started", "event: tool.finished", "event: token",
                         "event: message", "event: turn.finished"}) {
    EXPECT_NE(body.find(ev), std::string::npos) << ev << "\n" << body;
  }
  EXPECT_NE(body.find("Your day "), std::string::npos);
  EXPECT_NE(body.find(t["id"].asString()), std::string::npos);
}

TEST_F(Agent, AMessageSeenByTheRunningTurnIsMergedNotAnsweredTwice) {
  // Turn 1 takes three model calls; message 2 arrives while it runs and is in the context of its later calls.
  fake->script(J(R"([
    {"delay_ms":400,"content":"","tool_calls":[{"name":"context_now","arguments":{}}]},
    {"delay_ms":400,"content":"","tool_calls":[{"name":"context_now","arguments":{}}]},
    {"content":"Both noted."}])"));
  const auto first = send("first thing");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const auto second = send("second thing");
  EXPECT_EQ(waitTurn(first["id"].asString())["status"].asString(), "done");
  EXPECT_EQ(waitTurn(second["id"].asString())["status"].asString(), "merged");
  EXPECT_EQ(fake->requests().size(), 3u);
  const auto last = fake->requests()[2]["messages"];
  bool sawSecond = false;
  for (const auto& m : last) sawSecond |= m["content"].asString() == "second thing";
  EXPECT_TRUE(sawSecond);
}

TEST_F(Agent, AMessageTheRunningTurnNeverSawGetsItsOwnTurn) {
  fake->script(J(R"([{"delay_ms":500,"content":"Answer one."},{"content":"Answer two."}])"));
  const auto first = send("one");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const auto second = send("two");
  EXPECT_EQ(waitTurn(first["id"].asString())["status"].asString(), "done");
  EXPECT_EQ(waitTurn(second["id"].asString())["status"].asString(), "done");
  EXPECT_EQ(lastReply(), "Answer two.");
}

// ------------------------------------------------------------------------------------------------ notifications

TEST_F(Agent, DesktopNotificationsLongPollAndAck) {
  const auto dev = api->post("/devices", J(R"({"name":"laptop","platform":"linux-x11"})"));
  ASSERT_EQ(dev.status, 201) << dev.body;
  ApiClient device(*server, user, dev.json["token"].asString());

  const auto t0 = std::chrono::steady_clock::now();
  const auto empty = device.get("/devices/self/notifications?wait=2");
  EXPECT_EQ(empty.status, 200);
  EXPECT_EQ(empty.json["items"].size(), 0u);
  EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(1500));  // it waited

  const auto nid = ids::newUuidV7();
  sql(user.tenantId,
      "INSERT INTO notifications (id, tenant_id, user_id, kind, urgency, title, body, action) "
      "SELECT $1::uuid, tenant_id, id, 'reminder', 'normal', 'Call the bank', 'Due now', 'open_chat' FROM users",
      {nid});
  const auto got = device.get("/devices/self/notifications?wait=0");
  ASSERT_EQ(got.json["items"].size(), 1u);
  EXPECT_EQ(got.json["items"][0]["title"].asString(), "Call the bank");
  EXPECT_EQ(device.post("/devices/self/notifications/" + nid + ":ack",
                        J(R"({"shown_at":"2026-11-13T10:00:00Z","clicked":true})")).status, 204);
  EXPECT_EQ(device.get("/devices/self/notifications?wait=0").json["items"].size(), 0u);
  EXPECT_EQ(sql(user.tenantId, "SELECT resolved_at IS NOT NULL AS r FROM notifications")[0]["r"].as<bool>(), true);
  EXPECT_EQ(device.post("/devices/self/notifications/" + ids::newUuidV7() + ":ack",
                        J(R"({"shown_at":"2026-11-13T10:00:00Z","clicked":false})")).status, 404);
  EXPECT_EQ(api->get("/devices/self/notifications").status, 403);  // users are not devices
}
