// Proactivity end to end (agent_lld.md §4, §7.4, §8): reminders from templates, follow-ups and triggers as
// proactive turns, the Notifier gate (spacing, urgent, no_message, suppressed), routines, events from disputes and
// missed tasks, the entertainment scan, notifications and acks, and replies deferred while the model was offline.
// The scheduling function is called by the test (housekeeping off), and the worker only claims this tenant's jobs.
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

class Proactive : public ::testing::Test {
 protected:
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  TestUser user;

  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    user = createTenantWithUser();
    disableDefaultRoutines(user.tenantId);  // tests add their own, before this test's server claims jobs
    start();
    // Proactive messages hold during quiet hours unless the user was just active; a fresh log keeps the tests
    // independent of the time of day.
    Json::Value a = J(R"({"title":"Setup","area":"other"})");
    a["started_at"] = at(-5), a["ended_at"] = at(-4);
    ASSERT_EQ(api->post("/activities", a).status, 201);
    settle();
  }

  // Other test processes queue ticks for every user of the shared database at any moment, so a reply must be
  // scripted before anything becomes due (and before a server starts with something already due).
  void start(const Json::Value& script = Json::Value()) {
    fake = std::make_unique<FakeLlm>();
    if (!script.isNull()) fake->script(script);
    server = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake->baseUrl()},
                                {"THELIFE_PROACTIVE", "on"},
                                {"THELIFE_JOBS", "on"},
                                {"THELIFE_JOBS_TENANT", user.tenantId},
                                {"THELIFE_JOBS_HOUSEKEEPING", "off"},
                                {"THELIFE_JOBS_POLL_MS", "100"}});
    api = std::make_unique<ApiClient>(*server, user);
  }

  // RFC 3339 for now + minutes.
  std::string at(int minutes) {
    return sql(user.tenantId, "SELECT to_char(now() + make_interval(mins => $1::int), 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS t",
               {std::to_string(minutes)})[0]["t"].as<std::string>();
  }

  // One scheduling pass, then wait for the tick and the turns it started.
  // The scheduling function queues a tick only when something is due (scans run every 10 minutes); the test
  // also queues one directly so scans run now.
  void tick() {
    sql(user.tenantId, "SELECT ops.enqueue_proactive_ticks()");
    sql(user.tenantId,
        "INSERT INTO ops.jobs (id, tenant_id, kind, payload, dedupe_key, priority) SELECT gen_random_uuid(), tenant_id, "
        "'proactive.tick', jsonb_build_object('user_id', id), 'tick:' || id, 1 FROM users "
        "ON CONFLICT (kind, dedupe_key) WHERE dedupe_key IS NOT NULL AND status IN ('queued', 'running') DO NOTHING");
    settle();
  }
  void settle() {
    ASSERT_TRUE(eventually([&] {
      const auto r = sql(user.tenantId,
                         "SELECT (SELECT count(*) FROM ops.jobs WHERE tenant_id = $1::uuid AND kind = 'proactive.tick' "
                         "          AND status IN ('queued', 'running'))::int "
                         "     + (SELECT count(*) FROM turns WHERE status IN ('queued', 'running'))::int AS n",
                         {user.tenantId});
      return r[0]["n"].as<int>() == 0;
    }));
  }

  Json::Value messages() {
    const auto convs = api->get("/conversations?channel=web").json["items"];
    if (convs.empty()) return Json::Value(Json::arrayValue);
    return api->get("/conversations/" + convs[convs.size() - 1]["id"].asString() + "/messages").json["items"];
  }

  Json::Value lastAssistant() {
    const auto m = messages();
    for (int i = static_cast<int>(m.size()) - 1; i >= 0; --i) {
      if (m[i]["role"].asString() == "assistant" && m[i]["tool_calls"].empty()) return m[i];
    }
    return {};
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

  void followup(const std::string& reason) {
    sql(user.tenantId,
        "INSERT INTO agent_followups (id, tenant_id, user_id, due_at, reason) "
        "SELECT gen_random_uuid(), tenant_id, id, now() - interval '1 second', $1 FROM users",
        {reason});
  }
};

}  // namespace

TEST_F(Proactive, RemindersComeFromTemplatesWithoutTheModel) {
  Json::Value once = J(R"({"text":"Call the bank"})");
  once["due_at"] = at(-1);
  const auto r1 = api->post("/reminders", once);
  ASSERT_EQ(r1.status, 201) << r1.body;
  Json::Value daily = J(R"({"text":"Take vitamins","rrule":"FREQ=DAILY"})");
  daily["due_at"] = at(-1);
  const auto r2 = api->post("/reminders", daily);
  const auto task = api->post("/tasks", J(R"({"title":"Renew passport","area":"personal"})"));
  ASSERT_EQ(api->patch("/tasks/" + task.json["id"].asString(), J(R"({"status":"done"})"), "\"1\"").status, 200);
  Json::Value linked = J(R"({"text":"Renew passport","link_type":"task"})");
  linked["link_id"] = task.json["id"];
  linked["due_at"] = at(-1);
  const auto r3 = api->post("/reminders", linked);

  tick();
  EXPECT_EQ(api->get("/reminders/" + r1.json["id"].asString()).json["status"].asString(), "sent");
  const auto d = api->get("/reminders/" + r2.json["id"].asString()).json;
  EXPECT_EQ(d["status"].asString(), "pending");  // repeats: moved to tomorrow
  EXPECT_GT(d["due_at"].asString(), at(60 * 23));
  EXPECT_EQ(api->get("/reminders/" + r3.json["id"].asString()).json["status"].asString(), "cancelled");  // task done

  std::set<std::string> said;
  for (const auto& m : messages()) said.insert(m["content"].asString());
  EXPECT_TRUE(said.count("Reminder: Call the bank"));
  EXPECT_TRUE(said.count("Reminder: Take vitamins"));
  EXPECT_FALSE(said.count("Reminder: Renew passport"));
  const auto notes = api->get("/notifications?open=true").json["items"];
  EXPECT_EQ(notes.size(), 2u);
  EXPECT_EQ(notes[0]["kind"].asString(), "reminder");
  EXPECT_TRUE(fake->requests().empty());  // no model involved

  // Acknowledging resolves it.
  const auto ack = api->post("/notifications/" + notes[0]["id"].asString() + ":ack", J(R"({"clicked":true})"));
  ASSERT_EQ(ack.status, 200) << ack.body;
  EXPECT_FALSE(ack.json["resolved_at"].isNull());
  EXPECT_EQ(ack.json["deliveries"][0]["status"].asString(), "clicked");
  EXPECT_EQ(api->get("/notifications?open=true").json["items"].size(), 1u);
}

TEST_F(Proactive, FollowUpsStartTurnsAndTheGateSpacesThem) {
  // The agent schedules a follow-up in chat.
  Json::Value call;
  call["name"] = "agent_schedule_followup";
  call["arguments"]["at"] = at(60);
  call["arguments"]["reason"] = "ask how the interview went";
  Json::Value step;
  step["content"] = "";
  step["tool_calls"].append(call);
  Json::Value script(Json::arrayValue);
  script.append(step);
  script.append(J(R"({"content":"I'll check in after it."})"));
  fake->script(script);
  chat("interview at 3 today");
  const auto f = sql(user.tenantId, "SELECT reason, channel, conversation_id IS NOT NULL AS conv FROM agent_followups WHERE status = 'pending'");
  ASSERT_EQ(f.size(), 1u);
  EXPECT_TRUE(f[0]["conv"].as<bool>());
  fake->script(J(R"([{"content":"How did the interview go?"}])"));
  sql(user.tenantId, "UPDATE agent_followups SET due_at = now() WHERE status = 'pending'");
  tick();
  const auto sent = lastAssistant();
  EXPECT_EQ(sent["content"].asString(), "How did the interview go?");
  EXPECT_EQ(sent["kind"].asString(), "normal");
  const auto prompt = fake->requests()[2]["messages"];
  EXPECT_NE(prompt[prompt.size() - 1]["content"].asString().find("ask how the interview went"), std::string::npos);
  const auto n = api->get("/notifications").json["items"];
  ASSERT_EQ(n.size(), 1u);
  EXPECT_EQ(n[0]["kind"].asString(), "agent.followup");

  // A second proactive message within 45 minutes is held (kept in the transcript as suppressed)...
  fake->script(J(R"([{"content":"Did the report go out?"}])"));
  followup("check the report");
  tick();
  EXPECT_EQ(lastAssistant()["kind"].asString(), "suppressed");
  EXPECT_EQ(sql(user.tenantId, "SELECT error FROM turns WHERE trigger = 'proactive' ORDER BY created_at DESC LIMIT 1")[0]["error"]
                .as<std::string>(),
            "spacing");
  EXPECT_EQ(api->get("/notifications").json["items"].size(), 1u);

  // ...unless the agent marks it urgent.
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"notify_urgent","arguments":{}}]},
                    {"content":"The spec is due in an hour."}])"));
  followup("deliverable due in an hour");
  tick();
  EXPECT_EQ(lastAssistant()["kind"].asString(), "normal")
      << sql(user.tenantId, "SELECT coalesce(error, '-') AS e FROM turns WHERE trigger = 'proactive' ORDER BY created_at DESC LIMIT 1")[0]["e"].as<std::string>();
  // Second step of that turn: the event comes before the turn's own steps, so the model continues after the tool
  // result instead of seeing the request again (found live: it looped on context_now).
  const auto steps = fake->requests()[fake->requests().size() - 1]["messages"];
  int note = -1, urgentCall = -1;
  for (Json::ArrayIndex i = 0; i < steps.size(); ++i) {
    if (steps[i]["content"].asString().find("deliverable due in an hour") != std::string::npos) note = static_cast<int>(i);
    if (steps[i]["tool_calls"].isArray() && !steps[i]["tool_calls"].empty() &&
        steps[i]["tool_calls"][0]["function"]["name"].asString() == "notify_urgent") urgentCall = static_cast<int>(i);
  }
  ASSERT_GE(note, 0);
  EXPECT_LT(note, urgentCall);
  EXPECT_EQ(steps[note]["role"].asString(), "user");
  EXPECT_EQ(steps[steps.size() - 1]["role"].asString(), "tool");
  const auto urgent = api->get("/notifications").json["items"];
  ASSERT_EQ(urgent.size(), 2u);
  EXPECT_EQ(urgent[1]["urgency"].asString(), "critical");
}

TEST_F(Proactive, NoMessageEndsTheTurnWithoutAnotherModelCall) {
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"agent_no_message","arguments":{}}]}])"));
  followup("nothing important");
  tick();
  EXPECT_EQ(fake->requests().size(), 1u);
  const auto t = sql(user.tenantId, "SELECT status, error FROM turns WHERE trigger = 'proactive'")[0];
  EXPECT_EQ(t["status"].as<std::string>(), "done");
  EXPECT_EQ(t["error"].as<std::string>(), "no_message");
  EXPECT_EQ(api->get("/notifications").json["items"].size(), 0u);
}

TEST_F(Proactive, RoutinesFireOnceAtTheirTimeAndCanBeSkipped) {
  const auto now = sql(user.tenantId, "SELECT to_char(now() - interval '1 minute', 'HH24:MI') AS t")[0]["t"].as<std::string>();
  Json::Value body = J(R"({"kind":"custom","title":"Water the plants","mode":"fixed","instructions":"Ask if the plants were watered."})");
  body["default_time"] = now;
  fake->script(J(R"([{"content":"Have the plants had water today?"}])"));
  const auto r = api->post("/routines", body);
  ASSERT_EQ(r.status, 201) << r.body;
  tick();
  EXPECT_EQ(lastAssistant()["content"].asString(), "Have the plants had water today?");
  const auto req = fake->requests()[0]["messages"];
  EXPECT_NE(req[req.size() - 1]["content"].asString().find("Ask if the plants were watered."), std::string::npos);
  EXPECT_FALSE(api->get("/routines/" + r.json["id"].asString()).json["last_fired_on"].isNull());
  tick();
  EXPECT_EQ(fake->requests().size(), 1u);  // once a day

  // Skipping: "skip it today" from chat needs no approval (user_reported_progress) and nothing fires.
  sql(user.tenantId, "UPDATE routines SET last_fired_on = NULL");
  Json::Value call;
  call["name"] = "routines_skip_once";
  call["arguments"]["id"] = r.json["id"];
  Json::Value step;
  step["content"] = "";
  step["tool_calls"].append(call);
  Json::Value script(Json::arrayValue);
  script.append(step);
  script.append(J(R"({"content":"Skipped for today."})"));
  fake->script(script);
  chat("skip the plants reminder today");
  EXPECT_EQ(api->get("/approvals?status=pending").json["items"].size(), 0u);
  EXPECT_FALSE(api->get("/routines/" + r.json["id"].asString()).json["skip_on"].isNull());
  const auto before = fake->requests().size();
  tick();
  EXPECT_EQ(fake->requests().size(), before);
}

TEST_F(Proactive, DisputesStartASilentReviewAndMissedTasksATrigger) {
  Json::Value det = J(R"({"source":"user","area":"entertainment","category":"games","summary":"Gaming","confidence":0.9})");
  det["started_at"] = at(-60), det["ended_at"] = at(-30);
  const auto d = api->post("/detections", det);
  ASSERT_EQ(d.status, 201) << d.body;
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"agent_no_message","arguments":{}}]}])"));
  ASSERT_EQ(api->post("/detections/" + d.json["id"].asString() + ":dispute", J(R"({"reason":"I was reading docs"})")).status, 200);
  tick();
  ASSERT_EQ(fake->requests().size(), 1u);
  const auto req = fake->requests()[0];
  const auto msgs = req["messages"];
  EXPECT_NE(msgs[msgs.size() - 1]["content"].asString().find("silent review"), std::string::npos);
  EXPECT_NE(msgs[msgs.size() - 1]["content"].asString().find("I was reading docs"), std::string::npos);
  bool claimTool = false;
  for (const auto& t : req["tools"]) claimTool |= t["function"]["name"].asString() == "detections_history";
  EXPECT_TRUE(claimTool);  // the detections group is loaded for the review
  EXPECT_EQ(api->get("/notifications").json["items"].size(), 0u);

  fake->script(J(R"([{"content":"Journal and Stretch slipped yesterday. Move them to today?"}])"));
  sql(user.tenantId,
      "INSERT INTO events (id, tenant_id, user_id, kind, entity_type, payload) SELECT gen_random_uuid(), tenant_id, id, "
      "'task.missed', 'day', '{\"day\":\"2026-11-12\",\"titles\":[\"Journal\",\"Stretch\"]}' FROM users");
  tick();
  const auto t = sql(user.tenantId, "SELECT kind, status FROM agent_triggers ORDER BY created_at");
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[1]["kind"].as<std::string>(), "tasks_missed");
  EXPECT_EQ(t[1]["status"].as<std::string>(), "fired");
  EXPECT_NE(fake->requests()[1]["messages"][fake->requests()[1]["messages"].size() - 1]["content"].asString().find("Stretch"),
            std::string::npos);
  // The same day's misses never trigger twice.
  sql(user.tenantId,
      "INSERT INTO events (id, tenant_id, user_id, kind, entity_type, payload) SELECT gen_random_uuid(), tenant_id, id, "
      "'task.missed', 'day', '{\"day\":\"2026-11-12\",\"titles\":[\"Journal\"]}' FROM users");
  tick();
  EXPECT_EQ(fake->requests().size(), 2u);
}

TEST_F(Proactive, EntertainmentWhileSomethingIsDueStartsAWarning) {
  fake->script(J(R"([{"content":"The expense report is due in 30 minutes - wrap up this round and send it?"}])"));
  Json::Value game = J(R"({"title":"Gaming","area":"entertainment"})");
  game["started_at"] = at(-25), game["ended_at"] = at(0);
  ASSERT_EQ(api->post("/activities", game).status, 201);
  Json::Value task = J(R"({"title":"Submit the expense report","area":"work"})");
  task["due_at"] = at(30);
  ASSERT_EQ(api->post("/tasks", task).status, 201);
  tick();
  const auto t = sql(user.tenantId, "SELECT kind FROM agent_triggers");
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0]["kind"].as<std::string>(), "entertainment_warning");
  const auto req = fake->requests()[0]["messages"];
  EXPECT_NE(req[req.size() - 1]["content"].asString().find("Submit the expense report"), std::string::npos);
  tick();  // one warning per entertainment session
  EXPECT_EQ(fake->requests().size(), 1u);
}

TEST_F(Proactive, MessagesSavedWhileOfflineAreAnsweredAfterTheReset) {
  fake->script(J(R"([{"error":{"status":429,"body":{"error":"weekly usage limit reached"}}}])"));
  EXPECT_EQ(chat("I ran 5k this morning")["status"].asString(), "manual");
  chat("/help");  // commands are answered at once and never deferred
  // The quota is back (a fresh model endpoint here: provider state is kept per endpoint).
  server.reset();
  start(J(R"([{"content":"Nice run! Logged 5k this morning."}])"));
  tick();
  const auto d = sql(user.tenantId, "SELECT status FROM turns WHERE trigger = 'deferred_reply'");
  ASSERT_EQ(d.size(), 1u);
  EXPECT_EQ(d[0]["status"].as<std::string>(), "done");
  EXPECT_EQ(lastAssistant()["content"].asString(), "Nice run! Logged 5k this morning.");
  const auto msgs = fake->requests()[0]["messages"];
  bool note = false, userMsg = false;
  for (const auto& m : msgs) {
    note |= m["content"].asString().find("while you were offline") != std::string::npos;
    userMsg |= m["content"].asString() == "I ran 5k this morning";
  }
  EXPECT_TRUE(note && userMsg);
  tick();
  EXPECT_EQ(fake->requests().size(), 1u);  // answered once
}

TEST_F(Proactive, AProactiveTurnOnADayWithoutUserMessagesKeepsItsOwnSteps) {
  // Found live: context trimming skipped to the first user message, so on a quiet morning the routine turn lost its
  // own tool call and result each step and called context_now until the step limit.
  fake->script(J(R"([{"content":"","tool_calls":[{"name":"context_now","arguments":{}}]},{"content":"All clear today."}])"));
  followup("check the day");
  tick();
  ASSERT_EQ(fake->requests().size(), 2u);
  const auto second = fake->requests()[1]["messages"];
  bool call = false, result = false;
  for (const auto& m : second) {
    call |= m["tool_calls"].isArray() && !m["tool_calls"].empty();
    result |= m["role"].asString() == "tool" && m["content"].asString().find("local_day") != std::string::npos;
  }
  EXPECT_TRUE(call && result);
  EXPECT_EQ(lastAssistant()["content"].asString(), "All clear today.");
}
