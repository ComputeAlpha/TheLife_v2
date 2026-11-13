#include "llm/gateway.h"

#include <drogon/drogon.h>
#include <json/reader.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>

#include "core/blocking.h"
#include "core/env.h"
#include "core/json.h"
#include "tools/schema_validator.h"

namespace thelife::llm {

namespace {

trantor::ConcurrentTaskQueue& llmPool() {
  // Calls block for seconds to minutes; a small dedicated pool keeps them off the shared blocking pool.
  static trantor::ConcurrentTaskQueue pool(4, "llm");
  return pool;
}

int millisBetween(Clock::time_point a, Clock::time_point b) {
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
}

std::string errorsText(const std::vector<FieldError>& errs) {
  std::string out;
  for (const auto& e : errs) out += "- " + (e.path.empty() ? std::string("/") : e.path) + ": " + e.message + "\n";
  return out;
}

// The JSON object in a model reply: whole text, or inside ``` fences, or the outermost {...}.
std::optional<Json::Value> extractJsonObject(const std::string& text) {
  auto tryParse = [](const std::string& s) -> std::optional<Json::Value> {
    try {
      auto v = json::parse(s);
      if (v.isObject()) return v;
    } catch (...) {
    }
    return std::nullopt;
  };
  if (auto v = tryParse(text)) return v;
  if (const auto f = text.find("```"); f != std::string::npos) {
    const auto start = text.find('\n', f);
    const auto end = text.find("```", start == std::string::npos ? f + 3 : start);
    if (start != std::string::npos && end != std::string::npos) {
      if (auto v = tryParse(text.substr(start + 1, end - start - 1))) return v;
    }
  }
  const auto b = text.find('{');
  const auto e = text.rfind('}');
  if (b != std::string::npos && e != std::string::npos && e > b) return tryParse(text.substr(b, e - b + 1));
  return std::nullopt;
}

}  // namespace

Gateway::Gateway(GatewayConfig cfg, std::vector<std::unique_ptr<Provider>> providers,
                 std::shared_ptr<StateStore> store, std::shared_ptr<UsageSink> usage, GatewayHooks hooks)
    : cfg_(std::move(cfg)), store_(std::move(store)), usage_(std::move(usage)), hooks_(std::move(hooks)) {
  std::map<std::string, ProviderStatus> saved;
  if (store_) {
    for (auto& s : store_->load()) saved[s.provider] = s;
  }
  for (auto& p : providers) {
    Slot slot;
    slot.status.provider = p->config().name;
    slot.status.changedAt = now();
    if (auto it = saved.find(slot.status.provider); it != saved.end()) slot.status = it->second;
    // A probe interrupted by a restart is retried as exhausted-and-due.
    if (slot.status.state == "probing") slot.status.state = "exhausted";
    slot.provider = std::move(p);
    providers_.push_back(std::move(slot));
  }
}

Clock::time_point Gateway::now() const { return hooks_.now ? hooks_.now() : Clock::now(); }

void Gateway::sleep(std::chrono::milliseconds d) const {
  if (d.count() <= 0) return;
  if (hooks_.sleep) hooks_.sleep(d);
  else std::this_thread::sleep_for(d);
}

void Gateway::setState(Slot& s, std::string state, std::optional<Clock::time_point> until, std::string scope,
                       std::string reason) {
  if (s.status.state == state && s.status.until == until) return;
  s.status.state = std::move(state);
  s.status.until = until;
  s.status.quotaScope = std::move(scope);
  s.status.reason = std::move(reason);
  s.status.changedAt = now();
  LOG_WARN << "llm provider=" << s.status.provider << " state=" << s.status.state << " reason=" << s.status.reason;
  if (store_) {
    try {
      store_->save(s.status);
    } catch (const std::exception& e) {
      LOG_ERROR << "llm provider state not saved: " << e.what();
    }
  }
  if (hooks_.onStateChange) hooks_.onStateChange(s.status);
}

bool Gateway::capable(const Provider& p, const LlmRequest& req) const {
  const auto& c = p.config().caps;
  if (p.config().kind == "stub") return false;
  if (req.vision && !c.vision) return false;
  if (!req.tools.empty() && !c.tools) return false;
  return true;
}

// Expired throttles heal. Returns true when an exhausted provider is past its reset estimate and the caller
// should probe it (§3.4); the state is then "probing" so concurrent calls do not probe too. Caller holds mu_.
bool Gateway::refresh(Slot& s) {
  const auto t = now();
  if (s.status.state == "throttled" && s.status.until && t >= *s.status.until) {
    setState(s, "healthy", std::nullopt, "", "throttle expired");
  }
  if (s.status.state == "exhausted" && s.status.until && t >= *s.status.until) {
    setState(s, "probing", s.status.until, s.status.quotaScope, "reset time reached");
    return true;
  }
  return false;
}

// One cheap request, made without holding mu_.
void Gateway::probe(Slot& s) {
  LlmRequest req;
  req.purpose = "llm.probe";
  req.priority = Priority::Background;
  req.messages = {Message::user("Reply with: ok")};
  req.maxTokens = 16;
  req.reasoningEffort = "low";
  std::optional<LlmError> err;
  try {
    s.provider->complete(req, nullptr);
  } catch (const LlmError& e) {
    err = e;
  }
  std::lock_guard lock(mu_);
  if (!err || err->cls == ErrorClass::InvalidOutput) {  // a 200 with an odd body still proves the quota is back
    setState(s, "healthy", std::nullopt, "", "probe succeeded");
  } else if (err->cls == ErrorClass::AuthFailed) {
    setState(s, "disabled", std::nullopt, "", "auth failed during probe");
  } else {
    // Still limited (or unreachable): try again in an hour.
    setState(s, "exhausted", err->resetsAt.value_or(now() + std::chrono::hours(1)),
             err->quotaScope.empty() ? s.status.quotaScope : err->quotaScope,
             std::string("probe failed: ") + toString(err->cls));
  }
}

void Gateway::captureFixture(const LlmError& e) const {
  if (cfg_.fixtureDir.empty() || e.rawBody.empty()) return;
  static std::mutex m;
  static std::set<std::string> seen;
  const std::string key = std::to_string(e.status) + ":" + e.rawBody.substr(0, 80);
  std::lock_guard lock(m);
  if (!seen.insert(key).second) return;
  try {
    std::filesystem::create_directories(cfg_.fixtureDir);
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(now().time_since_epoch()).count();
    const auto path = std::filesystem::path(cfg_.fixtureDir) /
                      ("unclassified-" + std::to_string(e.status) + "-" + std::to_string(stamp) + ".json");
    Json::Value v;
    v["status"] = e.status;
    v["body"] = e.rawBody;
    std::ofstream(path) << json::compact(v) << '\n';
    LOG_WARN << "llm unclassified error body captured to " << path.string();
  } catch (const std::exception& ex) {
    LOG_ERROR << "llm fixture capture failed: " << ex.what();
  }
}

// One provider, with the retry rules of §3.3. Throws the final LlmError.
LlmResponse Gateway::attempt(Slot& s, const LlmRequest& req, StreamSink* sink, int& attempts) {
  int transient = 0, invalid = 0, limited = 0;
  std::mt19937_64 rng(std::random_device{}());
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  for (;;) {
    ++attempts;
    try {
      return s.provider->complete(req, sink);
    } catch (const LlmError& e) {
      if (sink) sink->reset();
      if (e.cls == ErrorClass::Transient && transient < cfg_.transientRetries) {
        // Full jitter: uniform in [0, min(20 s, 1 s · 2^n)].
        const double capMs = std::min(20000.0, 1000.0 * static_cast<double>(1 << transient));
        const double r = hooks_.random01 ? hooks_.random01() : uni(rng);
        ++transient;
        sleep(std::chrono::milliseconds(static_cast<long>(r * capMs)));
        continue;
      }
      if (e.cls == ErrorClass::RateLimited && req.priority == Priority::Interactive && limited < 2) {
        const int wait = e.retryAfterSec.value_or(5);
        if (wait <= cfg_.maxRateLimitWaitSec) {
          ++limited;
          sleep(std::chrono::seconds(wait));
          continue;
        }
      }
      if (e.cls == ErrorClass::InvalidOutput && invalid < 1) {
        ++invalid;
        continue;
      }
      if (e.cls == ErrorClass::Unclassified) captureFixture(e);
      throw;
    }
  }
}

LlmResponse Gateway::completeBlocking(const LlmRequest& req, StreamSink* sink) {
  if (req.tenantId.empty()) throw std::invalid_argument("LlmRequest.tenantId is required");
  const auto started = now();
  int attempts = 0;
  int images = 0;
  for (const auto& m : req.messages) images += static_cast<int>(m.imageDataUris.size());
  CallRecord rec{req.tenantId, req.purpose, "", "", req.promptId, req.promptVersion, req.priority,
                 req.conversationId, {}, images, 0, 0, std::nullopt};
  auto account = [&](const std::optional<ErrorClass>& err) {
    rec.attempts = std::max(attempts, 1);
    rec.latencyMs = std::max(0, millisBetween(started, now()));
    rec.error = err;
    if (usage_) {
      try {
        usage_->record(rec);
      } catch (const std::exception& ex) {
        LOG_ERROR << "llm usage not recorded: " << ex.what();
      }
    }
  };

  std::optional<LlmError> last;
  std::optional<Clock::time_point> resetsAt;
  std::string quotaScope;
  for (auto& s : providers_) {
    if (!capable(*s.provider, req)) continue;
    bool due = false;
    {
      std::lock_guard lock(mu_);
      due = refresh(s);
    }
    if (due) probe(s);
    rec.provider = s.provider->config().name;
    rec.model = req.vision ? s.provider->config().visionModel : s.provider->config().chatModel;
    ProviderStatus st;
    {
      std::lock_guard lock(mu_);
      st = s.status;
    }
    if (st.state == "disabled") {
      last = LlmError(ErrorClass::ProviderUnavailable, rec.provider + " disabled");
      continue;
    }
    if (st.state == "exhausted" || st.state == "probing") {
      resetsAt = st.until;
      quotaScope = st.quotaScope;
      last = LlmError(ErrorClass::ProviderUnavailable, rec.provider + " quota exhausted");
      continue;
    }
    if (st.state == "throttled" && st.until) {
      const auto waitMs = millisBetween(now(), *st.until);
      if (req.priority == Priority::Interactive && waitMs <= cfg_.maxRateLimitWaitSec * 1000) {
        sleep(std::chrono::milliseconds(waitMs));
      } else {
        LlmError e(ErrorClass::RateLimited, rec.provider + " throttled");
        e.retryAfterSec = std::max(1, waitMs / 1000);
        last = e;
        continue;
      }
    }
    try {
      auto resp = attempt(s, req, sink, attempts);
      resp.attempts = attempts;
      rec.usage = resp.usage;
      if (!resp.model.empty()) rec.model = resp.model;
      account(std::nullopt);
      return resp;
    } catch (const LlmError& e) {
      std::lock_guard lock(mu_);
      switch (e.cls) {
        case ErrorClass::QuotaExhausted: {
          Clock::time_point until;
          if (e.resetsAt) until = *e.resetsAt;
          else if (e.quotaScope == "weekly" || e.quotaScope == "unknown" || e.quotaScope.empty())
            until = store_ ? store_->nextWeeklyReset(cfg_.weeklyReset, now()) : now() + std::chrono::hours(24 * 7);
          else until = now() + std::chrono::hours(1);
          setState(s, "exhausted", until, e.quotaScope.empty() ? "unknown" : e.quotaScope, "quota exhausted");
          resetsAt = until;
          quotaScope = s.status.quotaScope;
          last = e;
          continue;
        }
        case ErrorClass::AuthFailed:
          setState(s, "disabled", std::nullopt, "", "authentication failed");
          last = e;
          continue;
        case ErrorClass::RateLimited:
          setState(s, "throttled", now() + std::chrono::seconds(std::clamp(e.retryAfterSec.value_or(30), 1, 3600)),
                   "", "rate limited");
          last = e;
          continue;
        default:
          account(e.cls);
          throw;
      }
    }
  }
  if (!last) last = LlmError(ErrorClass::ProviderUnavailable, providers_.empty() ? "no LLM provider configured"
                                                                                 : "no capable provider");
  if (attempts > 0) account(last->cls);
  // A rate limit is temporary and keeps its class (callers reschedule). Anything else that reaches here
  // (exhausted, disabled, nothing capable) means manual mode for the caller.
  if (last->cls == ErrorClass::RateLimited) throw *last;
  LlmError e(ErrorClass::ProviderUnavailable, last->cls == ErrorClass::ProviderUnavailable ? last->detail
                                                                                             : toString(last->cls));
  e.resetsAt = resetsAt;
  e.quotaScope = quotaScope;
  throw e;
}

Json::Value Gateway::structuredBlocking(const LlmRequest& base, const std::string& name, const Json::Value& schema) {
  const tools::SchemaValidator validator(schema);
  const std::string toolName = "submit_" + name;

  // 1. Tool-as-output (the target model ignores response_format; spikes/llm/REPORT.md).
  LlmRequest req = base;
  req.tools = {ToolSpec{toolName, "Submit the final " + name + " result. Call this exactly once.", schema}};
  req.toolChoice = "auto";
  req.messages.push_back(Message::system("When you have the result, call " + toolName +
                                         " exactly once with it. Do not answer in prose."));
  auto check = [&](const LlmResponse& r) -> std::pair<std::optional<Json::Value>, std::string> {
    for (const auto& c : r.toolCalls) {
      if (c.name != toolName) continue;
      Json::Value args;
      try {
        args = json::parse(c.arguments);
      } catch (const std::exception&) {
        return {std::nullopt, "the arguments were not valid JSON"};
      }
      auto errs = validator.validate(args);
      if (errs.empty()) return {args, ""};
      return {std::nullopt, errorsText(errs)};
    }
    return {std::nullopt, "you did not call " + toolName};
  };

  auto first = completeBlocking(req, nullptr);
  auto [ok, why] = check(first);
  if (ok) return *ok;

  // 2. One repair turn with the validation errors.
  req.messages.push_back(Message::assistant(first.content, first.toolCalls));
  bool answered = false;
  for (const auto& c : first.toolCalls) {
    req.messages.push_back(Message::tool(c.id, "Rejected. Fix these problems and call " + toolName + " again:\n" + why));
    answered = true;
  }
  if (!answered) {
    req.messages.push_back(Message::user("Your reply was rejected: " + why + ". Call " + toolName + " now."));
  }
  auto second = completeBlocking(req, nullptr);
  auto [ok2, why2] = check(second);
  if (ok2) return *ok2;

  // 3. Schema in the prompt, same validation.
  LlmRequest plain = base;
  plain.tools.clear();
  plain.messages.push_back(Message::system(
      "Reply with only one JSON object that matches this JSON Schema. No prose, no code fences.\n" +
      json::compact(schema)));
  auto third = completeBlocking(plain, nullptr);
  if (auto v = extractJsonObject(third.content)) {
    auto errs = validator.validate(*v);
    if (errs.empty()) return *v;
    throw LlmError(ErrorClass::InvalidOutput, "structured output '" + name + "' failed validation after repair", 200);
  }
  throw LlmError(ErrorClass::InvalidOutput, "structured output '" + name + "' was not JSON after repair", 200);
}

drogon::Task<LlmResponse> Gateway::complete(LlmRequest req, StreamSink* sink) {
  co_return co_await runBlockingOn(llmPool(), [this, r = std::move(req), sink] { return completeBlocking(r, sink); });
}

drogon::Task<Json::Value> Gateway::structured(LlmRequest req, std::string name, Json::Value schema) {
  co_return co_await runBlockingOn(llmPool(), [this, r = std::move(req), n = std::move(name), s = std::move(schema)] {
    return structuredBlocking(r, n, s);
  });
}

std::vector<ProviderStatus> Gateway::status() {
  std::lock_guard lock(mu_);
  std::vector<ProviderStatus> out;
  for (auto& s : providers_) {
    if (s.status.state == "throttled" && s.status.until && now() >= *s.status.until) {
      setState(s, "healthy", std::nullopt, "", "throttle expired");
    }
    out.push_back(s.status);
  }
  return out;
}

std::string Gateway::mode(std::optional<Clock::time_point>* resetsAt) {
  if (providers_.empty()) return "unconfigured";
  std::lock_guard lock(mu_);
  std::optional<Clock::time_point> earliest;
  for (auto& s : providers_) {
    if (s.provider->config().kind == "stub") continue;
    const auto& st = s.status.state;
    if (st == "healthy" || st == "throttled") return "online";
    // exhausted past its reset estimate: the next call will probe, so it counts as online
    if (st == "exhausted" && s.status.until && now() >= *s.status.until) return "online";
    if ((st == "exhausted" || st == "probing") && s.status.until) {
      if (!earliest || *s.status.until < *earliest) earliest = s.status.until;
    }
  }
  if (resetsAt) *resetsAt = earliest;
  return "manual";
}

// ------------------------------------------------------------------------------------------------ config

GatewayConfig parseConfig(const Json::Value& doc) {
  if (!doc.isObject()) throw std::invalid_argument("LLM config must be a JSON object");
  GatewayConfig cfg;
  const auto& providers = doc["providers"];
  if (!providers.isArray() || providers.empty()) throw std::invalid_argument("LLM config: providers[] is required");
  std::set<std::string> names;
  for (const auto& p : providers) {
    ProviderConfig pc;
    pc.name = p["name"].asString();
    if (pc.name.empty() || !names.insert(pc.name).second) throw std::invalid_argument("LLM config: provider names must be unique and non-empty");
    pc.kind = p.get("kind", "openai").asString();
    pc.role = p.get("role", "primary").asString();
    if (pc.kind != "openai" && pc.kind != "stub") throw std::invalid_argument("LLM config: unknown kind " + pc.kind);
    if (pc.role != "primary" && pc.role != "fallback") throw std::invalid_argument("LLM config: unknown role " + pc.role);
    if (pc.kind == "openai") {
      pc.baseUrl = p["base_url"].asString();
      while (!pc.baseUrl.empty() && pc.baseUrl.back() == '/') pc.baseUrl.pop_back();
      if (pc.baseUrl.rfind("http://", 0) != 0 && pc.baseUrl.rfind("https://", 0) != 0) {
        throw std::invalid_argument("LLM config: base_url must be http(s) for " + pc.name);
      }
      if (p["api_key_env"].isString() && !p["api_key_env"].asString().empty()) {
        pc.apiKey = env::get(p["api_key_env"].asString());
      }
      pc.chatModel = p["models"]["chat"].asString();
      pc.visionModel = p["models"].get("vision", pc.chatModel).asString();
      if (pc.chatModel.empty()) throw std::invalid_argument("LLM config: models.chat is required for " + pc.name);
      const auto& c = p["caps"];
      pc.caps.tools = c.get("tools", true).asBool();
      pc.caps.jsonSchema = c.get("json_schema", false).asBool();
      pc.caps.vision = c.get("vision", false).asBool();
      pc.caps.ctxTokens = c.get("ctx_tokens", 32768).asInt();
      pc.timeoutSec = p.get("timeout_s", 180).asInt();
      pc.idleTimeoutSec = p.get("idle_timeout_s", 60).asInt();
    }
    cfg.providers.push_back(std::move(pc));
  }
  std::stable_partition(cfg.providers.begin(), cfg.providers.end(),
                        [](const ProviderConfig& p) { return p.role == "primary"; });
  if (const auto& r = doc["weekly_reset"]; r.isObject()) {
    cfg.weeklyReset.isoWeekday = r.get("iso_weekday", 1).asInt();
    cfg.weeklyReset.hour = r.get("hour", 0).asInt();
    cfg.weeklyReset.timezone = r.get("timezone", "UTC").asString();
    if (cfg.weeklyReset.isoWeekday < 1 || cfg.weeklyReset.isoWeekday > 7 || cfg.weeklyReset.hour < 0 ||
        cfg.weeklyReset.hour > 23) {
      throw std::invalid_argument("LLM config: weekly_reset out of range");
    }
  }
  cfg.weeklyRequestEstimate = doc.get("weekly_request_estimate", 3000).asInt();
  return cfg;
}

namespace {
std::unique_ptr<Gateway>& slot() {
  static std::unique_ptr<Gateway> g;
  return g;
}
}  // namespace

Gateway& gateway() {
  auto& g = slot();
  if (!g) g = std::make_unique<Gateway>(GatewayConfig{}, std::vector<std::unique_ptr<Provider>>{}, nullptr, nullptr);
  return *g;
}

void setGateway(std::unique_ptr<Gateway> g) { slot() = std::move(g); }

}  // namespace thelife::llm
