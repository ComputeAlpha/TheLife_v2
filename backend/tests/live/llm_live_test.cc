// Live smoke against the real endpoint (agent_lld.md §12). Manual or nightly only: it spends real quota.
//   THELIFE_LIVE_LLM_URL=http://127.0.0.1:11434/v1 [THELIFE_LIVE_LLM_MODEL=glm-5.3-flash:cloud] thelife_live_tests
#include <gtest/gtest.h>

#include <cstdlib>

#include "core/json.h"
#include "llm/gateway.h"
#include "llm/provider.h"
#include "llm/transport.h"

using namespace thelife;
using namespace thelife::llm;

namespace {

std::string envOr(const char* k, const char* d) {
  const char* v = std::getenv(k);
  return v && *v ? v : d;
}

struct Sink : StreamSink {
  std::string shown;
  void delta(std::string_view t) override { shown.append(t); }
  void reset() override { shown.clear(); }
};

class LiveLlm : public ::testing::Test {
 protected:
  std::unique_ptr<Gateway> gw;
  void SetUp() override {
    const auto url = envOr("THELIFE_LIVE_LLM_URL", "");
    if (url.empty()) GTEST_SKIP() << "THELIFE_LIVE_LLM_URL not set (live smoke spends real quota)";
    ProviderConfig pc;
    pc.name = "live";
    pc.baseUrl = url;
    pc.chatModel = envOr("THELIFE_LIVE_LLM_MODEL", "glm-5.3-flash:cloud");
    pc.visionModel = pc.chatModel;
    pc.caps.vision = true;
    pc.timeoutSec = 120;
    std::vector<std::unique_ptr<Provider>> ps;
    ps.push_back(makeOpenAiProvider(pc, std::shared_ptr<Transport>(makeCurlTransport())));
    gw = std::make_unique<Gateway>(GatewayConfig{}, std::move(ps), nullptr, nullptr);
  }
  LlmRequest req() {
    LlmRequest r;
    r.tenantId = "live-smoke";
    r.purpose = "live.smoke";
    return r;
  }
};

}  // namespace

TEST_F(LiveLlm, StreamedTurn) {
  auto r = req();
  r.messages = {Message::user("Reply with exactly the word: pong")};
  Sink sink;
  const auto out = gw->completeBlocking(r, &sink);
  EXPECT_NE(out.content.find("pong"), std::string::npos) << out.content;
  EXPECT_EQ(sink.shown, out.content);
  EXPECT_GT(out.usage.tokensIn, 0);
  EXPECT_EQ(out.content.find("</think>"), std::string::npos);
}

TEST_F(LiveLlm, ToolCallWithCleanContent) {
  auto r = req();
  r.messages = {Message::system("You are TheLife's assistant. Current local time: 2026-11-13T14:05:00+05:30."),
                Message::user("Remind me at 6pm today to call the bank.")};
  r.tools = {ToolSpec{"reminders_create", "Create a reminder",
                      json::parse(R"({"type":"object","required":["text","due_at"],"properties":{
                        "text":{"type":"string"},"due_at":{"type":"string","format":"date-time"}}})")}};
  Sink sink;
  const auto out = gw->completeBlocking(r, &sink);
  ASSERT_EQ(out.toolCalls.size(), 1u) << out.content;
  EXPECT_EQ(out.toolCalls[0].name, "reminders_create");
  const auto args = json::parse(out.toolCalls[0].arguments);
  EXPECT_NE(args["due_at"].asString().find("18:00"), std::string::npos);
  EXPECT_EQ(out.content.find("</think>"), std::string::npos);
}

TEST_F(LiveLlm, StructuredOutput) {
  auto r = req();
  r.reasoningEffort = "low";
  r.messages = {Message::user("Window title: 'main.cc — thelife — Visual Studio Code'. Classify the activity.")};
  const auto v = gw->structuredBlocking(
      r, "classification",
      json::parse(R"({"type":"object","additionalProperties":false,"required":["area","confidence"],"properties":{
        "area":{"type":"string","enum":["work","skill","fitness","personal","entertainment","other"]},
        "confidence":{"type":"number","minimum":0,"maximum":1}}})"));
  EXPECT_TRUE(v["area"].asString() == "work" || v["area"].asString() == "skill") << json::compact(v);
}
