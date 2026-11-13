// LLM gateway without network (agent_lld.md §3): classifier against recorded Stage 0 bodies, response and SSE
// parsing, the provider state machine on a fake clock, retries, and structured output.
#include <gtest/gtest.h>

#include <deque>
#include <fstream>
#include <sstream>

#include "core/json.h"
#include "llm/classifier.h"
#include "llm/gateway.h"
#include "llm/provider.h"

using namespace thelife;
using namespace thelife::llm;
using namespace std::chrono_literals;

namespace {

Json::Value fixture(const std::string& name) {
  std::ifstream in(std::string(THELIFE_LLM_FIXTURES) + "/" + name + ".json");
  std::stringstream ss;
  ss << in.rdbuf();
  return json::parse(ss.str());
}

HttpResult recordedError(const std::string& key) {
  const auto fx = fixture("error_fixtures")[key];
  HttpResult r;
  r.status = fx["status"].asInt();
  r.body = json::compact(fx["body"]);
  for (const auto& name : fx["headers"].getMemberNames()) {
    std::string lower = name;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    r.headers[lower] = fx["headers"][name].asString();
  }
  return r;
}

HttpResult http(int status, std::string body, std::map<std::string, std::string> headers = {}) {
  return HttpResult{status, std::move(headers), std::move(body), ""};
}

const auto t0 = Clock::from_time_t(1794562600);  // 2026-11-13T09:36:40Z

}  // namespace

// ----------------------------------------------------------------------------------------------- classifier

TEST(LlmClassifier, RecordedStage0Bodies) {
  // JSON body with Content-Type text/html: still parsed, classified as a bad request (unknown model).
  auto e = classify(recordedError("unknown_model"), t0);
  EXPECT_EQ(e.cls, ErrorClass::BadRequest);
  EXPECT_EQ(e.status, 404);
  EXPECT_NE(e.detail.find("not_found_error"), std::string::npos);
  EXPECT_EQ(classify(recordedError("bad_tool_schema"), t0).cls, ErrorClass::BadRequest);

  // 200 with empty content and finish_reason "load": a silent failure.
  const auto parsed = parseChatResponse(fixture("error_fixtures")["bad_role"]["body"]);
  const auto bad = checkCompletion(parsed);
  ASSERT_TRUE(bad);
  EXPECT_EQ(bad->cls, ErrorClass::InvalidOutput);
}

TEST(LlmClassifier, EveryClass) {
  EXPECT_EQ(classify(HttpResult{0, {}, "", "Couldn't connect to server"}, t0).cls, ErrorClass::Transient);
  EXPECT_EQ(classify(http(503, "upstream down"), t0).cls, ErrorClass::Transient);
  EXPECT_EQ(classify(http(502, "<html>bad gateway</html>"), t0).cls, ErrorClass::Transient);

  auto rl = classify(http(429, R"({"error":{"message":"Too Many Requests"}})", {{"retry-after", "7"}}), t0);
  EXPECT_EQ(rl.cls, ErrorClass::RateLimited);
  EXPECT_EQ(rl.retryAfterSec, 7);

  auto weekly = classify(http(429, R"({"error":"you have reached your weekly usage limit, upgrade for higher limits"})"), t0);
  EXPECT_EQ(weekly.cls, ErrorClass::QuotaExhausted);
  EXPECT_EQ(weekly.quotaScope, "weekly");
  EXPECT_FALSE(weekly.resetsAt);

  auto hourly = classify(http(429, R"({"error":{"message":"hourly usage limit reached","resets_at":"2026-11-13T10:00:00Z"}})"), t0);
  EXPECT_EQ(hourly.cls, ErrorClass::QuotaExhausted);
  EXPECT_EQ(hourly.quotaScope, "hourly");
  ASSERT_TRUE(hourly.resetsAt);
  EXPECT_EQ(toRfc3339(*hourly.resetsAt), "2026-11-13T10:00:00Z");

  EXPECT_EQ(classify(http(402, R"({"error":{"message":"payment required"}})"), t0).cls, ErrorClass::QuotaExhausted);
  EXPECT_EQ(classify(http(403, R"({"error":{"message":"quota exceeded for this account"}})"), t0).cls,
            ErrorClass::QuotaExhausted);
  EXPECT_EQ(classify(http(401, R"({"error":{"message":"invalid api key"}})"), t0).cls, ErrorClass::AuthFailed);
  EXPECT_EQ(classify(http(403, R"({"error":{"message":"forbidden"}})"), t0).cls, ErrorClass::AuthFailed);
  EXPECT_EQ(classify(http(400, R"({"error":{"message":"prompt is too long: exceeds the maximum context length"}})"), t0).cls,
            ErrorClass::ContextTooLong);
  EXPECT_EQ(classify(http(400, R"({"error":{"message":"bad","type":"content_filter"}})"), t0).cls,
            ErrorClass::ContentFiltered);
  EXPECT_EQ(classify(http(422, "{}"), t0).cls, ErrorClass::BadRequest);

  auto odd = classify(http(418, R"({"error":{"message":"teapot"}})"), t0);
  EXPECT_EQ(odd.cls, ErrorClass::Unclassified);
  EXPECT_FALSE(odd.rawBody.empty());  // kept for fixture capture
}

TEST(LlmClassifier, Rfc3339RoundTrip) {
  EXPECT_EQ(toRfc3339(t0), "2026-11-13T09:36:40Z");
  EXPECT_EQ(parseRfc3339("2026-11-13T15:06:40+05:30"), t0);
  EXPECT_EQ(parseRfc3339("2026-11-13T09:36:40.250Z"), t0);
  EXPECT_FALSE(parseRfc3339("yesterday"));
}

// ----------------------------------------------------------------------------------------------- parsing

TEST(LlmParse, RecordedResponses) {
  const auto basic = parseChatResponse(fixture("chat_basic"));
  EXPECT_EQ(basic.content, "pong");
  EXPECT_FALSE(basic.reasoning.empty());
  EXPECT_EQ(basic.usage.tokensIn, 17);
  EXPECT_FALSE(checkCompletion(basic));

  // Tool calls arrive with reasoning leaked into content, ending in </think>: it must not reach the user.
  const auto single = parseChatResponse(fixture("tools_single"));
  ASSERT_EQ(single.toolCalls.size(), 1u);
  EXPECT_EQ(single.toolCalls[0].name, "reminders_create");
  EXPECT_EQ(json::parse(single.toolCalls[0].arguments)["text"].asString(), "Call the bank");
  EXPECT_EQ(single.content, "");
  EXPECT_NE(single.reasoning.find("reminder"), std::string::npos);
  EXPECT_FALSE(checkCompletion(single));

  const auto parallel = parseChatResponse(fixture("tools_parallel"));
  EXPECT_EQ(parallel.toolCalls.size(), 3u);
  EXPECT_EQ(parallel.finishReason, "tool_calls");
}

TEST(LlmParse, ChatBodyNeverSendsReasoningNone) {
  LlmRequest r;
  r.messages = {Message::system("s"), Message::user("u")};
  r.reasoningEffort = "none";
  r.tools = {ToolSpec{"tasks_create", "d", json::parse(R"({"type":"object"})")}};
  const auto b = buildChatBody(r, "m", true);
  EXPECT_FALSE(b.isMember("reasoning_effort"));
  EXPECT_TRUE(b["stream_options"]["include_usage"].asBool());
  EXPECT_EQ(b["tools"][0]["function"]["name"].asString(), "tasks_create");
  r.reasoningEffort = "low";
  EXPECT_EQ(buildChatBody(r, "m", false)["reasoning_effort"].asString(), "low");

  Message img = Message::user("what is on screen?");
  img.imageDataUris = {"data:image/jpeg;base64,AAAA"};
  r.messages = {img};
  const auto v = buildChatBody(r, "m", false);
  EXPECT_EQ(v["messages"][0]["content"][1]["image_url"]["url"].asString(), "data:image/jpeg;base64,AAAA");
}

namespace {
struct RecordingSink : StreamSink {
  std::string shown;
  int resets = 0;
  void delta(std::string_view t) override { shown.append(t); }
  void reset() override {
    shown.clear();
    ++resets;
  }
};

std::string sse(const Json::Value& chunk) { return "data: " + json::compact(chunk) + "\n\n"; }
}  // namespace

TEST(LlmParse, SseStreamWithSplitLinesUsageAndToolCalls) {
  RecordingSink sink;
  SseAccumulator acc(&sink);
  std::string all;
  for (const auto& c : fixture("streaming")) all += sse(c);
  Json::Value content = json::parse(R"({"choices":[{"index":0,"delta":{"content":"Hello there"},"finish_reason":null}]})");
  Json::Value call1 = json::parse(
      R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"tasks_create","arguments":"{\"title\":"}}]}}]})");
  Json::Value call2 = json::parse(
      R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"x\"}"}}]}}]})");
  all = sse(content) + sse(call1) + sse(call2) + all + "data: [DONE]\n\n";
  for (std::size_t i = 0; i < all.size(); i += 7) acc.feed(std::string_view(all).substr(i, 7));  // split anywhere
  const auto r = acc.finish();
  EXPECT_EQ(sink.shown, "Hello there");
  EXPECT_EQ(r.content, "Hello there");
  ASSERT_EQ(r.toolCalls.size(), 1u);
  EXPECT_EQ(r.toolCalls[0].arguments, R"({"title":"x"})");
  EXPECT_EQ(r.finishReason, "stop");
  EXPECT_EQ(r.usage.tokensIn, 25);
  EXPECT_EQ(r.usage.tokensOut, 59);
}

TEST(LlmParse, SseRetractsLeakedReasoning) {
  RecordingSink sink;
  SseAccumulator acc(&sink);
  for (const char* piece : {"The user wants ", "a reminder.</th", "ink>Done: ", "reminder set."}) {
    Json::Value c;
    c["choices"][0]["delta"]["content"] = piece;
    acc.feed(sse(c));
  }
  acc.feed(R"(data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})" "\n\n");
  const auto r = acc.finish();
  EXPECT_EQ(sink.resets, 1);
  EXPECT_EQ(sink.shown, "Done: reminder set.");
  EXPECT_EQ(r.content, "Done: reminder set.");
  EXPECT_EQ(r.reasoning, "The user wants a reminder.");
}

TEST(LlmParse, SseEndingEarlyIsTransient) {
  SseAccumulator acc(nullptr);
  acc.feed(R"(data: {"choices":[{"index":0,"delta":{"content":"half"}}]})" "\n\n");
  try {
    acc.finish();
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::Transient);
  }
}

// ----------------------------------------------------------------------------------------------- gateway

namespace {

using Step = std::function<LlmResponse(const LlmRequest&, StreamSink*)>;

class ScriptedProvider : public Provider {
 public:
  explicit ScriptedProvider(std::string name = "primary", bool vision = true) {
    cfg_.name = std::move(name);
    cfg_.chatModel = "m";
    cfg_.visionModel = "mv";
    cfg_.caps.vision = vision;
  }
  const ProviderConfig& config() const override { return cfg_; }
  LlmResponse complete(const LlmRequest& req, StreamSink* sink) override {
    seen.push_back(req);
    if (steps.empty()) throw std::logic_error("script exhausted");
    auto f = steps.front();
    steps.pop_front();
    return f(req, sink);
  }
  std::deque<Step> steps;
  std::vector<LlmRequest> seen;

 private:
  ProviderConfig cfg_;
};

Step ok(std::string content) {
  return [content](const LlmRequest&, StreamSink* sink) {
    if (sink) sink->delta(content);
    LlmResponse r;
    r.content = content;
    r.finishReason = "stop";
    r.usage = {12, 3};
    return r;
  };
}
Step call(std::string name, std::string args) {
  return [name, args](const LlmRequest&, StreamSink*) {
    LlmResponse r;
    r.toolCalls = {ToolCallOut{"c" + std::to_string(args.size()), name, args}};
    r.finishReason = "tool_calls";
    return r;
  };
}
Step fail(ErrorClass cls, std::optional<int> retryAfter = std::nullopt, std::string scope = "",
          std::optional<Clock::time_point> resets = std::nullopt) {
  return [=](const LlmRequest&, StreamSink* sink) -> LlmResponse {
    if (sink) sink->delta("partial ");
    LlmError e(cls, "scripted");
    e.retryAfterSec = retryAfter;
    e.quotaScope = scope;
    e.resetsAt = resets;
    throw e;
  };
}

struct MemStore : StateStore {
  std::vector<ProviderStatus> saved;
  Clock::time_point weeklyReset = t0 + 72h;
  std::vector<ProviderStatus> load() override { return saved; }
  void save(const ProviderStatus& s) override {
    for (auto& x : saved) {
      if (x.provider == s.provider) {
        x = s;
        return;
      }
    }
    saved.push_back(s);
  }
  Clock::time_point nextWeeklyReset(const WeeklyReset&, Clock::time_point) override { return weeklyReset; }
};

struct MemSink : UsageSink {
  std::vector<CallRecord> records;
  void record(const CallRecord& r) override { records.push_back(r); }
};

struct Fixture {
  Clock::time_point now = t0;
  std::vector<std::chrono::milliseconds> slept;
  std::vector<ProviderStatus> changes;
  std::shared_ptr<MemStore> store = std::make_shared<MemStore>();
  std::shared_ptr<MemSink> sink = std::make_shared<MemSink>();
  ScriptedProvider* primary = nullptr;
  ScriptedProvider* second = nullptr;
  std::unique_ptr<Gateway> gw;

  explicit Fixture(bool withSecond = false, bool stubFallback = true) {
    std::vector<std::unique_ptr<Provider>> ps;
    auto p = std::make_unique<ScriptedProvider>();
    primary = p.get();
    ps.push_back(std::move(p));
    if (withSecond) {
      auto q = std::make_unique<ScriptedProvider>("secondary");
      second = q.get();
      ps.push_back(std::move(q));
    }
    GatewayConfig cfg;
    if (stubFallback) {
      ProviderConfig stub;
      stub.name = "fallback";
      stub.kind = "stub";
      stub.role = "fallback";
      ps.push_back(makeStubProvider(stub));
    }
    GatewayHooks hooks;
    hooks.now = [this] { return now; };
    hooks.sleep = [this](std::chrono::milliseconds d) {
      slept.push_back(d);
      now += d;
    };
    hooks.random01 = [] { return 0.5; };
    hooks.onStateChange = [this](const ProviderStatus& s) { changes.push_back(s); };
    gw = std::make_unique<Gateway>(cfg, std::move(ps), store, sink, hooks);
  }

  LlmRequest req(Priority p = Priority::Interactive) {
    LlmRequest r;
    r.tenantId = "00000000-0000-7000-8000-000000000001";
    r.purpose = "agent.turn";
    r.priority = p;
    r.messages = {Message::user("hi")};
    return r;
  }

  ErrorClass errorOf(const LlmRequest& r, LlmError* out = nullptr) {
    try {
      gw->completeBlocking(r);
    } catch (const LlmError& e) {
      if (out) *out = e;
      return e.cls;
    }
    ADD_FAILURE() << "expected an LlmError";
    return ErrorClass::Unclassified;
  }
};

}  // namespace

TEST(LlmGateway, TransientRetriesThreeTimesWithFullJitterThenSucceeds) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::Transient), fail(ErrorClass::Transient), fail(ErrorClass::Transient), ok("hello")};
  RecordingSink sink;
  const auto r = f.gw->completeBlocking(f.req(), &sink);
  EXPECT_EQ(r.content, "hello");
  EXPECT_EQ(r.attempts, 4);
  EXPECT_EQ(sink.shown, "hello");  // partial output of failed attempts was retracted
  EXPECT_EQ(sink.resets, 3);
  // jitter 0.5 × min(20 s, 1 s · 2^n)
  EXPECT_EQ(f.slept, (std::vector<std::chrono::milliseconds>{500ms, 1000ms, 2000ms}));
  ASSERT_EQ(f.sink->records.size(), 1u);
  EXPECT_EQ(f.sink->records[0].attempts, 4);
  EXPECT_FALSE(f.sink->records[0].error);
  EXPECT_EQ(f.sink->records[0].usage.tokensIn, 12);
}

TEST(LlmGateway, TransientGivesUpAfterFourAttempts) {
  Fixture f;
  for (int i = 0; i < 4; ++i) f.primary->steps.push_back(fail(ErrorClass::Transient));
  EXPECT_EQ(f.errorOf(f.req()), ErrorClass::Transient);
  EXPECT_EQ(f.primary->seen.size(), 4u);
  ASSERT_EQ(f.sink->records.size(), 1u);
  EXPECT_EQ(f.sink->records[0].error, ErrorClass::Transient);
  EXPECT_EQ(f.gw->status()[0].state, "healthy");  // transient failures do not change provider state
}

TEST(LlmGateway, WeeklyQuotaGoesManualUntilResetThenProbesBack) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::QuotaExhausted, std::nullopt, "weekly")};
  LlmError e(ErrorClass::Unclassified, "");
  EXPECT_EQ(f.errorOf(f.req(), &e), ErrorClass::ProviderUnavailable);
  ASSERT_TRUE(e.resetsAt);
  EXPECT_EQ(*e.resetsAt, f.store->weeklyReset);  // Monday 00:00 local from the store
  EXPECT_EQ(e.quotaScope, "weekly");
  EXPECT_EQ(f.gw->status()[0].state, "exhausted");
  ASSERT_EQ(f.store->saved.size(), 1u);  // persisted
  EXPECT_EQ(f.store->saved[0].state, "exhausted");
  std::optional<Clock::time_point> until;
  EXPECT_EQ(f.gw->mode(&until), "manual");
  EXPECT_EQ(until, f.store->weeklyReset);

  // Before the reset nothing is sent to the provider.
  f.now += 24h;
  EXPECT_EQ(f.errorOf(f.req()), ErrorClass::ProviderUnavailable);
  EXPECT_EQ(f.primary->seen.size(), 1u);

  // After the reset: a cheap probe, then the real call.
  f.now = f.store->weeklyReset + 1s;
  EXPECT_EQ(f.gw->mode(), "online");
  f.primary->steps = {ok("ok"), ok("back")};
  EXPECT_EQ(f.gw->completeBlocking(f.req()).content, "back");
  ASSERT_EQ(f.primary->seen.size(), 3u);
  EXPECT_EQ(f.primary->seen[1].purpose, "llm.probe");
  EXPECT_EQ(f.primary->seen[1].maxTokens, 16);
  EXPECT_EQ(f.gw->status()[0].state, "healthy");
}

TEST(LlmGateway, FailedProbeWaitsAnotherHour) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::QuotaExhausted, std::nullopt, "weekly")};
  f.errorOf(f.req());
  f.now = f.store->weeklyReset + 1s;
  f.primary->steps = {fail(ErrorClass::QuotaExhausted, std::nullopt, "weekly")};
  LlmError e(ErrorClass::Unclassified, "");
  EXPECT_EQ(f.errorOf(f.req(), &e), ErrorClass::ProviderUnavailable);
  EXPECT_EQ(*e.resetsAt, f.now + 1h);
  EXPECT_EQ(f.gw->status()[0].state, "exhausted");
}

TEST(LlmGateway, ProviderResetHintWinsOverWeeklyRule) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::QuotaExhausted, std::nullopt, "hourly", t0 + 20min)};
  LlmError e(ErrorClass::Unclassified, "");
  f.errorOf(f.req(), &e);
  EXPECT_EQ(*e.resetsAt, t0 + 20min);
}

TEST(LlmGateway, QuotaOnPrimaryRoutesToCapableFallback) {
  Fixture f(/*withSecond=*/true);
  f.primary->steps = {fail(ErrorClass::QuotaExhausted, std::nullopt, "weekly")};
  f.second->steps = {ok("from secondary")};
  const auto r = f.gw->completeBlocking(f.req());
  EXPECT_EQ(r.content, "from secondary");
  EXPECT_EQ(f.sink->records.back().provider, "secondary");
}

TEST(LlmGateway, StateSurvivesRestart) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::QuotaExhausted, std::nullopt, "weekly")};
  f.errorOf(f.req());
  // New gateway over the same store: still exhausted, no provider call.
  std::vector<std::unique_ptr<Provider>> ps;
  auto p = std::make_unique<ScriptedProvider>();
  auto* raw = p.get();
  ps.push_back(std::move(p));
  GatewayHooks hooks;
  hooks.now = [&] { return f.now; };
  Gateway again(GatewayConfig{}, std::move(ps), f.store, f.sink, hooks);
  EXPECT_EQ(again.mode(), "manual");
  try {
    again.completeBlocking(f.req());
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::ProviderUnavailable);
  }
  EXPECT_TRUE(raw->seen.empty());
}

TEST(LlmGateway, AuthFailureDisablesWithoutRetry) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::AuthFailed)};
  EXPECT_EQ(f.errorOf(f.req()), ErrorClass::ProviderUnavailable);
  EXPECT_EQ(f.primary->seen.size(), 1u);
  EXPECT_EQ(f.gw->status()[0].state, "disabled");
  EXPECT_EQ(f.gw->mode(), "manual");
  EXPECT_EQ(f.errorOf(f.req()), ErrorClass::ProviderUnavailable);
  EXPECT_EQ(f.primary->seen.size(), 1u);
}

TEST(LlmGateway, RateLimitWaitedOutForInteractiveRescheduledForBackground) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::RateLimited, 3), ok("after wait")};
  EXPECT_EQ(f.gw->completeBlocking(f.req()).content, "after wait");
  EXPECT_EQ(f.slept, (std::vector<std::chrono::milliseconds>{3000ms}));

  f.primary->steps = {fail(ErrorClass::RateLimited, 30)};
  LlmError e(ErrorClass::Unclassified, "");
  EXPECT_EQ(f.errorOf(f.req(Priority::Background), &e), ErrorClass::RateLimited);
  EXPECT_EQ(f.gw->status()[0].state, "throttled");
  // While throttled, background work is refused without a call; interactive waits out the remaining time.
  EXPECT_EQ(f.errorOf(f.req(Priority::Background)), ErrorClass::RateLimited);
  EXPECT_EQ(f.primary->seen.size(), 3u);
  f.primary->steps = {ok("waited")};
  EXPECT_EQ(f.gw->completeBlocking(f.req()).content, "waited");
  EXPECT_EQ(f.gw->status()[0].state, "healthy");
}

TEST(LlmGateway, InvalidOutputRetriedOnceAndContextTooLongNotRetried) {
  Fixture f;
  f.primary->steps = {fail(ErrorClass::InvalidOutput), ok("fine")};
  EXPECT_EQ(f.gw->completeBlocking(f.req()).content, "fine");
  f.primary->steps = {fail(ErrorClass::InvalidOutput), fail(ErrorClass::InvalidOutput)};
  EXPECT_EQ(f.errorOf(f.req()), ErrorClass::InvalidOutput);
  f.primary->steps = {fail(ErrorClass::ContextTooLong)};
  EXPECT_EQ(f.errorOf(f.req()), ErrorClass::ContextTooLong);
  EXPECT_EQ(f.gw->status()[0].state, "healthy");
}

TEST(LlmGateway, VisionNeedsACapableProvider) {
  std::vector<std::unique_ptr<Provider>> ps;
  ps.push_back(std::make_unique<ScriptedProvider>("textonly", /*vision=*/false));
  Gateway gw(GatewayConfig{}, std::move(ps), nullptr, nullptr);
  LlmRequest r;
  r.tenantId = "t";
  r.vision = true;
  try {
    gw.completeBlocking(r);
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::ProviderUnavailable);
  }
}

TEST(LlmGateway, UnconfiguredIsManual) {
  Gateway gw(GatewayConfig{}, {}, nullptr, nullptr);
  EXPECT_EQ(gw.mode(), "unconfigured");
  LlmRequest r;
  r.tenantId = "t";
  EXPECT_THROW(gw.completeBlocking(r), LlmError);
}

// ----------------------------------------------------------------------------------------------- structured

namespace {
const char* kSchema = R"({"type":"object","additionalProperties":false,"required":["area","confidence"],
  "properties":{"area":{"type":"string","enum":["work","skill","entertainment"]},
                "confidence":{"type":"number","minimum":0,"maximum":1}}})";
}

TEST(LlmStructured, ToolAsOutputFirstTry) {
  Fixture f;
  f.primary->steps = {call("submit_classification", R"({"area":"work","confidence":0.9})")};
  const auto v = f.gw->structuredBlocking(f.req(Priority::Vision), "classification", json::parse(kSchema));
  EXPECT_EQ(v["area"].asString(), "work");
  ASSERT_EQ(f.primary->seen.size(), 1u);
  ASSERT_EQ(f.primary->seen[0].tools.size(), 1u);
  EXPECT_EQ(f.primary->seen[0].tools[0].name, "submit_classification");
}

TEST(LlmStructured, RepairTurnCarriesValidationErrors) {
  Fixture f;
  // The Stage 0 failure mode: invented field names.
  f.primary->steps = {call("submit_classification", R"({"areas":["work"],"confidence":0.9})"),
                      call("submit_classification", R"({"area":"skill","confidence":0.7})")};
  const auto v = f.gw->structuredBlocking(f.req(), "classification", json::parse(kSchema));
  EXPECT_EQ(v["area"].asString(), "skill");
  ASSERT_EQ(f.primary->seen.size(), 2u);
  const auto& repair = f.primary->seen[1].messages;
  ASSERT_GE(repair.size(), 2u);
  EXPECT_EQ(repair.back().role, "tool");
  EXPECT_NE(repair.back().content.find("/areas"), std::string::npos);
  EXPECT_EQ(f.sink->records.size(), 2u);
}

TEST(LlmStructured, FallsBackToSchemaInPromptThenFails) {
  Fixture f;
  f.primary->steps = {ok("I think it is work."), ok("Still prose."),
                      ok("```json\n{\"area\":\"entertainment\",\"confidence\":0.6}\n```")};
  const auto v = f.gw->structuredBlocking(f.req(), "classification", json::parse(kSchema));
  EXPECT_EQ(v["area"].asString(), "entertainment");
  EXPECT_TRUE(f.primary->seen[2].tools.empty());
  EXPECT_NE(f.primary->seen[2].messages.back().content.find("JSON Schema"), std::string::npos);

  f.primary->steps = {ok("no"), ok("no"), ok(R"({"area":"nap","confidence":2})")};
  try {
    f.gw->structuredBlocking(f.req(), "classification", json::parse(kSchema));
    FAIL();
  } catch (const LlmError& e) {
    EXPECT_EQ(e.cls, ErrorClass::InvalidOutput);
  }
}

// ----------------------------------------------------------------------------------------------- config

TEST(LlmConfig, ParsesRepoConfigAndRejectsBadInput) {
  std::ifstream in(std::string(THELIFE_REPO_ROOT) + "/config/llm.json");
  ASSERT_TRUE(in) << "config/llm.json missing";
  std::stringstream ss;
  ss << in.rdbuf();
  const auto cfg = parseConfig(json::parse(ss.str()));
  ASSERT_GE(cfg.providers.size(), 2u);
  EXPECT_EQ(cfg.providers[0].role, "primary");
  EXPECT_EQ(cfg.providers[0].chatModel, "glm-5.3-flash:cloud");
  EXPECT_FALSE(cfg.providers[0].caps.jsonSchema);  // measured in the Stage 0 spike
  EXPECT_EQ(cfg.providers.back().kind, "stub");
  EXPECT_EQ(cfg.weeklyReset.isoWeekday, 1);

  EXPECT_THROW(parseConfig(json::parse(R"({"providers":[]})")), std::invalid_argument);
  EXPECT_THROW(parseConfig(json::parse(R"({"providers":[{"name":"a","base_url":"ftp://x","models":{"chat":"m"}}]})")),
               std::invalid_argument);
  EXPECT_THROW(parseConfig(json::parse(
                   R"({"providers":[{"name":"a","base_url":"http://x/v1","models":{"chat":"m"}},{"name":"a","kind":"stub"}]})")),
               std::invalid_argument);
}
