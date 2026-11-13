// Telegram channel end to end (agent_lld.md §7.2, §8) against a fake Bot API: linking by one-time code, dropping
// unlinked and group chats, turns on the telegram channel with replies in the chat, deterministic commands,
// approvals with signed buttons (and forged ones refused), notification routing when the user is away, unlinking.
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <random>
#include <thread>

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
  auto r = args.empty() ? tx->execSqlSync(q) : tx->execSqlSync(q, args[0]);
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

// Chat ids are unique per live link across the shared database: each test takes a random one.
int64_t randomChat() {
  static std::mt19937_64 rng(std::random_device{}());
  return static_cast<int64_t>(rng() % 9000000000LL) + 1000000000LL;
}

class Telegram : public ::testing::Test {
 protected:
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<FakeTelegram> tg;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  TestUser user;
  int64_t chat = 0;

  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    user = createTenantWithUser();
    chat = randomChat();
    fake = std::make_unique<FakeLlm>();
    tg = std::make_unique<FakeTelegram>();
    server = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake->baseUrl()},
                                {"THELIFE_TELEGRAM_BOT_TOKEN", FakeTelegram::kToken},
                                {"THELIFE_TELEGRAM_API", tg->apiBase()},
                                {"THELIFE_TELEGRAM_POLL_S", "1"},
                                {"THELIFE_PROACTIVE", "on"},  // ticks run only when the test queues one
                                {"THELIFE_JOBS", "on"},
                                {"THELIFE_JOBS_TENANT", user.tenantId},
                                {"THELIFE_JOBS_HOUSEKEEPING", "off"},
                                {"THELIFE_JOBS_POLL_MS", "100"}});
    api = std::make_unique<ApiClient>(*server, user);
  }

  // Texts the bot sent to this test's chat, oldest first.
  std::vector<std::string> sent() {
    std::vector<std::string> out;
    for (const auto& b : tg->calls("sendMessage")) {
      if (b["chat_id"].asInt64() == chat) out.push_back(b["text"].asString());
    }
    return out;
  }
  bool waitSent(std::size_t n) {
    return eventually([&] { return sent().size() >= n; });
  }

  void link() {
    const auto code = api->post("/telegram/link-code", Json::Value(Json::objectValue));
    ASSERT_EQ(code.status, 200) << code.body;
    std::string lower = code.json["code"].asString();
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto before = sent().size();
    tg->message(chat, "/link " + lower);  // case does not matter
    ASSERT_TRUE(waitSent(before + 1));
    ASSERT_NE(sent().back().find("Linked"), std::string::npos) << sent().back();
  }

  // Waits for every turn of this user to finish.
  void settle() {
    ASSERT_TRUE(eventually([&] {
      return sql(user.tenantId, "SELECT count(*)::int AS n FROM turns WHERE status IN ('queued', 'running')")[0]["n"]
                 .as<int>() == 0;
    }));
  }
};

}  // namespace

TEST_F(Telegram, LinkingByCodeAndOnlyLinkedPrivateChatsAreServed) {
  auto st = api->get("/telegram/link");
  ASSERT_EQ(st.status, 200) << st.body;
  EXPECT_TRUE(st.json["enabled"].asBool());
  EXPECT_FALSE(st.json["linked"].asBool());

  // Unlinked: /start explains, other messages are dropped without a reply, a wrong code is refused.
  tg->message(chat, "/start");
  ASSERT_TRUE(waitSent(1));
  EXPECT_NE(sent()[0].find("/link"), std::string::npos);
  tg->message(chat, "hello?");
  tg->message(chat, "/link NOTACODE");
  ASSERT_TRUE(waitSent(2));
  EXPECT_NE(sent()[1].find("not valid"), std::string::npos);
  EXPECT_EQ(sent().size(), 2u);  // "hello?" got nothing

  // A code is single use and replaced by a newer one.
  const auto old = api->post("/telegram/link-code", Json::Value(Json::objectValue)).json["code"].asString();
  link();
  tg->message(chat + 1, "/link " + old);
  EXPECT_TRUE(eventually([&] {
    for (const auto& b : tg->calls("sendMessage")) {
      if (b["chat_id"].asInt64() == chat + 1) return true;
    }
    return false;
  }));
  for (const auto& b : tg->calls("sendMessage")) {
    if (b["chat_id"].asInt64() == chat + 1) {
      EXPECT_NE(b["text"].asString().find("not valid"), std::string::npos);
    }
  }

  st = api->get("/telegram/link");
  EXPECT_TRUE(st.json["linked"].asBool());
  EXPECT_EQ(st.json["username"].asString(), "tester");
  EXPECT_FALSE(st.json["linked_at"].isNull());
  // The code is stored hashed only.
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*)::int AS n FROM telegram_link_codes WHERE code_hash = $1", {old})[0]["n"]
                .as<int>(),
            0);

  // A group chat with the same sender is never answered.
  Json::Value group;
  group["message"]["message_id"] = 77;
  group["message"]["chat"]["id"] = static_cast<Json::Int64>(-chat);
  group["message"]["chat"]["type"] = "group";
  group["message"]["from"]["id"] = static_cast<Json::Int64>(chat);
  group["message"]["text"] = "hi all";
  tg->push(group);

  // Unlinking stops the chat being served.
  const auto before = sent().size();
  ASSERT_EQ(api->del("/telegram/link", "").status, 204);
  EXPECT_FALSE(api->get("/telegram/link").json["linked"].asBool());
  tg->message(chat, "still there?");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_EQ(sent().size(), before);
  for (const auto& b : tg->calls("sendMessage")) EXPECT_NE(b["chat_id"].asInt64(), -chat);
  EXPECT_TRUE(fake->requests().empty());  // nothing reached the model
}

TEST_F(Telegram, MessagesRunTurnsOnTheTelegramChannelAndRepliesGoToTheChat) {
  link();
  fake->script(J(R"([{"content":"Logged: 40 min gym."}])"));
  const auto before = sent().size();
  tg->message(chat, "did 40 min gym");
  ASSERT_TRUE(waitSent(before + 1));
  settle();
  EXPECT_EQ(sent().back(), "Logged: 40 min gym.");
  EXPECT_FALSE(tg->calls("sendChatAction").empty());  // "typing…" while the turn ran

  // The exchange lives in the telegram conversation, not the web one.
  const auto convs = api->get("/conversations?channel=telegram").json["items"];
  ASSERT_EQ(convs.size(), 1u);
  const auto msgs = api->get("/conversations/" + convs[0]["id"].asString() + "/messages").json["items"];
  ASSERT_EQ(msgs.size(), 2u);
  EXPECT_EQ(msgs[0]["content"].asString(), "did 40 min gym");
  EXPECT_TRUE(api->get("/conversations?channel=web").json["items"].empty());
  // The model was told the channel.
  EXPECT_NE(json::compact(fake->requests()[0]).find("Channel: telegram"), std::string::npos);

  // Deterministic commands answer without the model.
  const auto calls = fake->requests().size();
  tg->message(chat, "/help");
  ASSERT_TRUE(waitSent(before + 2));
  settle();
  EXPECT_NE(sent().back().find("/approvals"), std::string::npos) << sent().back();
  EXPECT_EQ(fake->requests().size(), calls);

  // Long replies are split under Telegram's 4096-character limit, at line breaks.
  std::string longText;
  for (int i = 0; i < 120; ++i) longText += "Line " + std::to_string(i) + ": " + std::string(60, 'x') + "\n";
  longText += "End.";
  Json::Value script(Json::arrayValue);
  Json::Value step;
  step["content"] = longText;
  script.append(step);
  fake->script(script);
  const auto n0 = sent().size();
  tg->message(chat, "give me the long version");
  ASSERT_TRUE(waitSent(n0 + 1));
  settle();
  const auto all = sent();
  ASSERT_GE(all.size(), n0 + 3);
  std::string joined;
  for (auto i = n0; i < all.size(); ++i) {
    EXPECT_LE(all[i].size(), 4096u);
    joined += all[i];
  }
  EXPECT_EQ(joined, longText);
}

TEST_F(Telegram, ApprovalButtonsAreSignedAndDecideTheApproval) {
  link();
  const auto task = api->post("/tasks", J(R"({"title":"Gym 3x per week","area":"personal"})"));
  ASSERT_EQ(task.status, 201) << task.body;
  const auto id = task.json["id"].asString();
  fake->script(J((R"([
    {"content":"","tool_calls":[{"name":"tasks_update","arguments":{"id":")" + id + R"(","title":"Gym 4x per week"}}]},
    {"content":"That needs your approval."},
    {"content":"Done: gym is 4x a week now."}])").c_str()));
  const auto before = sent().size();
  tg->message(chat, "make gym 4 times a week");
  ASSERT_TRUE(waitSent(before + 2));
  settle();

  // The approval came with two buttons.
  Json::Value keyboard;
  for (const auto& b : tg->calls("sendMessage")) {
    if (b["chat_id"].asInt64() == chat && b.isMember("reply_markup")) keyboard = b["reply_markup"]["inline_keyboard"][0];
  }
  ASSERT_EQ(keyboard.size(), 2u);
  const auto approve = keyboard[0]["callback_data"].asString();
  const auto reject = keyboard[1]["callback_data"].asString();
  EXPECT_LE(approve.size(), 64u);
  EXPECT_EQ(approve.substr(0, 3), "ap:");

  // Forged buttons do nothing: a changed signature, the reject button's signature on approve, another chat.
  std::string forged = approve;
  forged.back() = forged.back() == 'A' ? 'B' : 'A';
  tg->callback(chat, forged);
  tg->callback(chat, approve.substr(0, 42) + reject.substr(42));
  tg->callback(chat + 7, approve);
  ASSERT_TRUE(eventually([&] { return tg->calls("answerCallbackQuery").size() >= 3; }));
  for (const auto& a : tg->calls("answerCallbackQuery")) EXPECT_EQ(a["text"].asString(), "This button is not valid.");
  EXPECT_EQ(api->get("/tasks/" + id).json["title"].asString(), "Gym 3x per week");

  // The real one approves; the agent's follow-up comes back to the chat.
  const auto beforeApprove = sent().size();
  tg->callback(chat, approve);
  ASSERT_TRUE(eventually([&] { return tg->calls("answerCallbackQuery").size() >= 4; }));
  EXPECT_EQ(tg->calls("answerCallbackQuery")[3]["text"].asString(), "Approved and done.");
  EXPECT_EQ(api->get("/tasks/" + id).json["title"].asString(), "Gym 4x per week");
  EXPECT_FALSE(tg->calls("editMessageText").empty());
  ASSERT_TRUE(waitSent(beforeApprove + 1));
  settle();
  EXPECT_EQ(sent().back(), "Done: gym is 4x a week now.");

  // Pressing it again changes nothing.
  tg->callback(chat, approve);
  ASSERT_TRUE(eventually([&] { return tg->calls("answerCallbackQuery").size() >= 5; }));
  EXPECT_NE(tg->calls("answerCallbackQuery")[4]["text"].asString().find("already"), std::string::npos);
}

TEST_F(Telegram, NotificationsGoToTelegramWhenTheUserIsAway) {
  link();
  // A due reminder (template path, no model) while no web tab is open and no collector is fresh: Telegram at once.
  Json::Value r = J(R"({"text":"Call the bank"})");
  r["due_at"] = sql(user.tenantId,
                    "SELECT to_char(now() - interval '1 minute', 'YYYY-MM-DD\"T\"HH24:MI:SS\"+00:00\"') AS t")[0]["t"]
                    .as<std::string>();
  ASSERT_EQ(api->post("/reminders", r).status, 201);
  const auto before = sent().size();
  sql(user.tenantId,
      "INSERT INTO ops.jobs (id, tenant_id, kind, payload, priority) SELECT gen_random_uuid(), tenant_id, "
      "'proactive.tick', jsonb_build_object('user_id', id), 1 FROM users");
  ASSERT_TRUE(waitSent(before + 1));
  EXPECT_EQ(sent().back(), "Reminder: Call the bank");
  EXPECT_TRUE(eventually([&] {
    const auto d = api->get("/notifications").json["items"];
    if (d.empty()) return false;
    for (const auto& x : d[0]["deliveries"]) {
      if (x["channel"].asString() == "telegram" && x["status"].asString() == "sent") return true;
    }
    return false;
  }));
  // The message was copied into the Telegram conversation, so a reply there has it in context.
  const auto convs = api->get("/conversations?channel=telegram").json["items"];
  ASSERT_EQ(convs.size(), 1u);
  const auto msgs = api->get("/conversations/" + convs[0]["id"].asString() + "/messages").json["items"];
  bool copied = false;
  for (const auto& m : msgs) copied = copied || m["content"].asString() == "Reminder: Call the bank";
  EXPECT_TRUE(copied);

  // An acked notification is not sent later: a queued fallback is skipped.
  sql(user.tenantId,
      "WITH n AS (INSERT INTO notifications (id, tenant_id, user_id, kind, urgency, title, body, action, proactive, "
      "  resolved_at) SELECT gen_random_uuid(), tenant_id, id, 'test', 'normal', 'Old', 'Already seen', 'open_chat', "
      "  false, now() FROM users RETURNING id, tenant_id) "
      "INSERT INTO ops.jobs (id, tenant_id, kind, payload, priority) SELECT gen_random_uuid(), tenant_id, "
      "'notify.telegram', jsonb_build_object('notification_id', id), 1 FROM n");
  EXPECT_TRUE(eventually([&] {
    return sql(user.tenantId,
               "SELECT count(*)::int AS n FROM ops.jobs WHERE tenant_id = $1::uuid AND kind = 'notify.telegram' "
               "AND status <> 'done'",
               {user.tenantId})[0]["n"]
               .as<int>() == 0;
  }));
  for (const auto& t : sent()) EXPECT_EQ(t.find("Already seen"), std::string::npos);
}

TEST_F(Telegram, EachChannelSeesTheOthersDigestOfToday) {
  link();
  // On Telegram the user logs something (a write): a digest of that channel is made in the background.
  fake->script(J(R"([
    {"content":"","tool_calls":[{"name":"tasks_create","arguments":{"title":"Call the bank","area":"personal"}}]},
    {"content":"Added: call the bank."},
    {"content":"","tool_calls":[{"name":"submit_channel_digest","arguments":{"digest":"Added a task to call the bank."}}]}])"));
  const auto before = sent().size();
  tg->message(chat, "remind me I need to call the bank");
  ASSERT_TRUE(waitSent(before + 1));
  ASSERT_TRUE(eventually([&] {
    return !sql(user.tenantId, "SELECT 1 FROM channel_digests WHERE channel = 'telegram'").empty();
  }, 15000));
  const auto d = sql(user.tenantId, "SELECT content, messages_seen FROM channel_digests WHERE channel = 'telegram'");
  EXPECT_EQ(d[0]["content"].as<std::string>(), "Added a task to call the bank.");
  EXPECT_EQ(d[0]["messages_seen"].as<int>(), 2);
  // The digest job saw the Telegram transcript, not the web one.
  const auto reqs = fake->requests();
  EXPECT_NE(json::compact(reqs[2]).find("call the bank"), std::string::npos);

  // On the web, the agent's context carries that digest; a read-only turn makes no new digest.
  fake->script(J(R"([{"content":"Yes, the bank call is on your list."}])"));
  Json::Value b;
  b["text"] = "anything I said earlier?";
  const auto turn = api->post("/chat/web/messages", b);
  ASSERT_EQ(turn.status, 202) << turn.body;
  ASSERT_TRUE(eventually([&] { return fake->requests().size() >= 4; }));
  settle();
  EXPECT_NE(json::compact(fake->requests()[3]).find("Today on the telegram channel"), std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_TRUE(sql(user.tenantId, "SELECT 1 FROM ops.jobs WHERE kind = 'digest.refresh' AND payload->>'channel' = 'web'").empty());
}
