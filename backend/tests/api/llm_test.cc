// LLM gateway over real HTTP (libcurl) against docker/fake-llm, and GET /system/llm-status on the real binary.
#include <gtest/gtest.h>

#include "core/ids.h"
#include "core/json.h"
#include "db/database.h"
#include "db/llm_store.h"
#include "llm/gateway.h"
#include "llm/provider.h"
#include "llm/transport.h"
#include "support/api_client.h"
#include "support/fake_llm.h"

using namespace thelife;
using namespace thelife::llm;
using thelife::test::FakeLlm;

namespace {

struct Sink : StreamSink {
  std::string shown;
  int resets = 0;
  void delta(std::string_view t) override { shown.append(t); }
  void reset() override {
    shown.clear();
    ++resets;
  }
};

struct MemUsage : UsageSink {
  std::vector<CallRecord> records;
  void record(const CallRecord& r) override { records.push_back(r); }
};

class LlmOverHttp : public ::testing::Test {
 protected:
  FakeLlm fake;
  std::shared_ptr<MemUsage> usage = std::make_shared<MemUsage>();
  std::unique_ptr<Gateway> gw;

  void SetUp() override {
    ProviderConfig pc;
    pc.name = "fake";
    pc.baseUrl = fake.baseUrl();
    pc.chatModel = "glm-5.3-flash:cloud";
    pc.visionModel = pc.chatModel;
    pc.caps.vision = true;
    pc.timeoutSec = 10;
    pc.idleTimeoutSec = 5;
    std::vector<std::unique_ptr<Provider>> ps;
    ps.push_back(makeOpenAiProvider(pc, std::shared_ptr<Transport>(makeCurlTransport())));
    GatewayHooks hooks;
    hooks.sleep = [](std::chrono::milliseconds) {};  // no real backoff in tests
    gw = std::make_unique<Gateway>(GatewayConfig{}, std::move(ps), nullptr, usage, hooks);
  }

  LlmRequest req() {
    LlmRequest r;
    r.tenantId = "00000000-0000-7000-8000-000000000001";
    r.messages = {Message::system("You are terse."), Message::user("say hello")};
    r.tools = {ToolSpec{"tasks_create", "Create a task", json::parse(R"({"type":"object","properties":{"title":{"type":"string"}}})")}};
    return r;
  }
};

}  // namespace

TEST_F(LlmOverHttp, StreamsContentAndUsage) {
  fake.script(json::parse(R"([{"content":"hello there, friend","usage":{"prompt_tokens":40,"completion_tokens":6}}])"));
  Sink sink;
  const auto r = gw->completeBlocking(req(), &sink);
  EXPECT_EQ(r.content, "hello there, friend");
  EXPECT_EQ(sink.shown, "hello there, friend");
  EXPECT_EQ(r.usage.tokensIn, 40);
  EXPECT_EQ(r.usage.tokensOut, 6);

  const auto sent = fake.requests()[0];
  EXPECT_EQ(sent["model"].asString(), "glm-5.3-flash:cloud");
  EXPECT_TRUE(sent["stream"].asBool());
  EXPECT_EQ(sent["tools"][0]["function"]["name"].asString(), "tasks_create");
  EXPECT_FALSE(sent.isMember("reasoning_effort"));
}

TEST_F(LlmOverHttp, StreamedToolCallsAreReassembled) {
  fake.script(json::parse(
      R"([{"content":"","tool_calls":[{"name":"tasks_create","arguments":{"title":"Renew passport"}},{"name":"tasks_create","arguments":{"title":"Call bank"}}]}])"));
  Sink sink;
  const auto r = gw->completeBlocking(req(), &sink);
  ASSERT_EQ(r.toolCalls.size(), 2u);
  EXPECT_EQ(json::parse(r.toolCalls[0].arguments)["title"].asString(), "Renew passport");
  EXPECT_EQ(json::parse(r.toolCalls[1].arguments)["title"].asString(), "Call bank");
  EXPECT_FALSE(r.toolCalls[0].id.empty());
  EXPECT_EQ(r.finishReason, "tool_calls");
}

TEST_F(LlmOverHttp, RecordedFixturesReplay) {
  fake.script(json::parse(R"([{"fixture":"tools_single"}])"));
  const auto r = gw->completeBlocking(req());
  ASSERT_EQ(r.toolCalls.size(), 1u);
  EXPECT_EQ(r.toolCalls[0].name, "reminders_create");
  EXPECT_EQ(r.content, "");  // leaked reasoning stripped

  fake.script(json::parse(R"([{"error_fixture":"unknown_model"}])"));
  try {
    gw->completeBlocking(req());
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::BadRequest);  // JSON body served as text/html
    EXPECT_EQ(e.status, 404);
  }
  EXPECT_EQ(usage->records.back().error, ErrorClass::BadRequest);

  fake.script(json::parse(R"([{"error_fixture":"bad_role"},{"error_fixture":"bad_role"}])"));
  try {
    gw->completeBlocking(req());
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::InvalidOutput);  // 200 + finish_reason "load", retried once
  }
  EXPECT_EQ(fake.requests().size(), 4u);
}

TEST_F(LlmOverHttp, DroppedConnectionsAndBrokenStreamsAreRetried) {
  fake.script(json::parse(R"([{"drop":true},{"content":"one two three four","break_after":2},{"content":"recovered fine"}])"));
  Sink sink;
  const auto r = gw->completeBlocking(req(), &sink);
  EXPECT_EQ(r.content, "recovered fine");
  EXPECT_EQ(r.attempts, 3);
  EXPECT_EQ(sink.shown, "recovered fine");  // the half-streamed attempt was retracted
  EXPECT_GE(sink.resets, 2);
}

TEST_F(LlmOverHttp, WeeklyLimitSwitchesToManualMode) {
  fake.script(json::parse(
      R"([{"error":{"status":429,"body":{"error":"you've reached your weekly usage limit. Upgrade for higher limits."}}}])"));
  try {
    gw->completeBlocking(req());
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::ProviderUnavailable);
    EXPECT_TRUE(e.resetsAt);
    EXPECT_EQ(e.quotaScope, "weekly");
  }
  EXPECT_EQ(gw->mode(), "manual");
  EXPECT_THROW(gw->completeBlocking(req()), LlmError);
  EXPECT_EQ(fake.requests().size(), 1u);  // nothing more is sent until the reset
}

TEST_F(LlmOverHttp, StructuredOutputThroughSubmitTool) {
  fake.script(json::parse(
      R"([{"tool_calls":[{"name":"submit_verdict","arguments":{"areas":["work"]}}]},
          {"tool_calls":[{"name":"submit_verdict","arguments":{"area":"work","confidence":0.8}}]}])"));
  const auto v = gw->structuredBlocking(
      req(), "verdict",
      json::parse(R"({"type":"object","additionalProperties":false,"required":["area","confidence"],
                      "properties":{"area":{"type":"string"},"confidence":{"type":"number"}}})"));
  EXPECT_EQ(v["area"].asString(), "work");
  const auto sent = fake.requests();
  ASSERT_EQ(sent.size(), 2u);
  const auto& repairMsgs = sent[1]["messages"];
  EXPECT_EQ(repairMsgs[repairMsgs.size() - 1]["role"].asString(), "tool");
}

// ----------------------------------------------------------------------------------------------- endpoint

TEST(LlmStatusEndpoint, ReportsModeProvidersAndUsage) {
  THELIFE_SKIP_WITHOUT_DB();
  FakeLlm fake;
  auto server = thelife::test::startServerWithDb({{"THELIFE_LLM_BASE_URL", fake.baseUrl()}});
  const auto user = thelife::test::createTenantWithUser();
  thelife::test::ApiClient api(*server, user);
  const auto r = api.get("/system/llm-status");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["mode"].asString(), "online");
  EXPECT_TRUE(r.json["resets_at"].isNull());
  ASSERT_EQ(r.json["providers"].size(), 1u);
  EXPECT_EQ(r.json["providers"][0]["state"].asString(), "healthy");
  EXPECT_EQ(r.json["providers"][0]["model"].asString(), "glm-5.3-flash:cloud");
  EXPECT_EQ(r.json["usage"]["requests"].asInt(), 0);
  EXPECT_EQ(r.json["usage"]["weekly_request_estimate"].asInt(), 3000);
  EXPECT_EQ(r.json["usage"]["week_start"].asString().size(), 10u);

  EXPECT_EQ(thelife::test::httpGet(server->baseUrl() + "/api/v1/system/llm-status").status, 401);
}

TEST(LlmStatusEndpoint, UnconfiguredGatewayIsReported) {
  THELIFE_SKIP_WITHOUT_DB();
  auto server = thelife::test::startServerWithDb();
  thelife::test::ApiClient api(*server, thelife::test::createTenantWithUser());
  const auto r = api.get("/system/llm-status");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["mode"].asString(), "unconfigured");
  EXPECT_EQ(r.json["providers"].size(), 0u);
}

// ----------------------------------------------------------------------------------------------- DB store

TEST(LlmDbStore, PersistsStateComputesLocalResetAndRecordsUsage) {
  THELIFE_SKIP_WITHOUT_DB();
  thelife::db::Database::init(thelife::test::appDbUrl(), 1, 5.0);
  auto store = thelife::db::makeDbStateStore();

  // Friday 2026-11-13 15:06 IST → next Monday 00:00 IST is 2026-11-15T18:30:00Z.
  const auto fri = Clock::from_time_t(1794562600);
  EXPECT_EQ(toRfc3339(store->nextWeeklyReset(WeeklyReset{1, 0, "Asia/Kolkata"}, fri)), "2026-11-15T18:30:00Z");
  // Exactly at the reset instant, the next one is a week later.
  const auto mon = *parseRfc3339("2026-11-15T18:30:00Z");
  EXPECT_EQ(toRfc3339(store->nextWeeklyReset(WeeklyReset{1, 0, "Asia/Kolkata"}, mon)), "2026-11-22T18:30:00Z");

  ProviderStatus s;
  s.provider = "test-" + thelife::ids::newUuidV7().substr(24);
  s.state = "exhausted";
  s.until = mon;
  s.quotaScope = "weekly";
  s.reason = "quota exhausted";
  store->save(s);
  bool found = false;
  for (const auto& x : store->load()) {
    if (x.provider != s.provider) continue;
    found = true;
    EXPECT_EQ(x.state, "exhausted");
    EXPECT_EQ(x.until, mon);
    EXPECT_EQ(x.quotaScope, "weekly");
  }
  EXPECT_TRUE(found);

  const auto user = thelife::test::createTenantWithUser();
  auto sink = thelife::db::makeDbUsageSink();
  CallRecord ok{user.tenantId, "agent.turn", "ollama", "glm-5.3-flash:cloud", "agent.system", "1.0.0",
                Priority::Interactive, std::nullopt, Usage{120, 30}, 0, 2, 840, std::nullopt};
  CallRecord bad = ok;
  bad.usage = {};
  bad.error = ErrorClass::Transient;
  sink->record(ok);
  sink->record(bad);

  auto tx = thelife::test::appDb()->newTransaction();
  tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", user.tenantId);
  const auto calls = tx->execSqlSync("SELECT status, error_class, tokens_in, attempts FROM llm_calls ORDER BY id");
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0]["status"].as<std::string>(), "ok");
  EXPECT_EQ(calls[0]["tokens_in"].as<int>(), 120);
  EXPECT_EQ(calls[1]["error_class"].as<std::string>(), "transient");
  const auto use = tx->execSqlSync("SELECT requests, failures, tokens_in, tokens_out FROM llm_usage");
  ASSERT_EQ(use.size(), 1u);
  EXPECT_EQ(use[0]["requests"].as<int>(), 2);
  EXPECT_EQ(use[0]["failures"].as<int>(), 1);
  EXPECT_EQ(use[0]["tokens_in"].as<long long>(), 120);
  tx->rollback();
  // Append-only: the app role cannot rewrite call history.
  auto tx2 = thelife::test::appDb()->newTransaction();
  tx2->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", user.tenantId);
  EXPECT_THROW(tx2->execSqlSync("UPDATE llm_calls SET tokens_in = 0"), std::exception);
  tx2->rollback();
}
