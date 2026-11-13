#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "llm/provider.h"
#include "llm/types.h"

namespace thelife::llm {

// When a weekly quota resets, absent a hint from the provider (agent_lld.md §3.4): weekday 1 = Monday.
struct WeeklyReset {
  int isoWeekday = 1;
  int hour = 0;
  std::string timezone = "UTC";
};

struct GatewayConfig {
  std::vector<ProviderConfig> providers;  // order matters: primary first
  WeeklyReset weeklyReset;
  int weeklyRequestEstimate = 3000;       // soft warning only (§3.5)
  std::string fixtureDir;                 // fixture-capture mode: unclassified error bodies are stored here
  int transientRetries = 3;
  int maxRateLimitWaitSec = 60;
};

struct ProviderStatus {
  std::string provider;  // configured name (display)
  // Storage identity: name + hash of endpoint and model. Quota state belongs to an endpoint, so pointing a name at
  // another endpoint (or a test's fake server) starts from fresh state.
  std::string key;
  std::string state = "healthy";  // healthy | throttled | exhausted | probing | disabled
  std::optional<Clock::time_point> until;
  std::string quotaScope;
  std::string reason;
  Clock::time_point changedAt{};
};

// Persists provider state across restarts and answers time-zone questions (the DB does both in production).
class StateStore {
 public:
  virtual ~StateStore() = default;
  virtual std::vector<ProviderStatus> load() = 0;
  virtual void save(const ProviderStatus& s) = 0;
  virtual Clock::time_point nextWeeklyReset(const WeeklyReset& rule, Clock::time_point now) = 0;
};

struct CallRecord {
  std::string tenantId, purpose, provider, model, promptId, promptVersion;
  Priority priority = Priority::Background;
  std::optional<std::string> conversationId;
  Usage usage;
  int images = 0;
  int attempts = 0;
  int latencyMs = 0;
  std::optional<ErrorClass> error;
};

class UsageSink {
 public:
  virtual ~UsageSink() = default;
  virtual void record(const CallRecord& r) = 0;
};

// Injection points for tests (time, sleeping, jitter, state-change notifications).
struct GatewayHooks {
  std::function<Clock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
  std::function<double()> random01;
  std::function<void(const ProviderStatus&)> onStateChange;
};

// The only path to a model (agent_lld.md §2–3). Blocking methods run on the caller's thread; the coroutine
// wrappers move them onto the gateway's own thread pool so event loops never block on a model.
class Gateway {
 public:
  Gateway(GatewayConfig cfg, std::vector<std::unique_ptr<Provider>> providers, std::shared_ptr<StateStore> store,
          std::shared_ptr<UsageSink> usage, GatewayHooks hooks = {});

  bool configured() const { return !providers_.empty(); }
  const GatewayConfig& config() const { return cfg_; }

  // Routes to the first capable provider that can take the call, retrying per the error taxonomy (§3.3).
  // Throws LlmError. ProviderUnavailable (with resetsAt when known) means "manual mode": no model is usable.
  LlmResponse completeBlocking(const LlmRequest& req, StreamSink* sink = nullptr);

  // Structured output (§3.2): tool-as-output → one repair turn → schema-in-prompt → InvalidOutput.
  // Returns the validated JSON object.
  Json::Value structuredBlocking(const LlmRequest& req, const std::string& name, const Json::Value& schema);

  drogon::Task<LlmResponse> complete(LlmRequest req, StreamSink* sink = nullptr);
  drogon::Task<Json::Value> structured(LlmRequest req, std::string name, Json::Value schema);

  std::vector<ProviderStatus> status();
  // "online", "manual" (no usable provider; resetsAt says until when, if known) or "unconfigured".
  std::string mode(std::optional<Clock::time_point>* resetsAt = nullptr);

 private:
  struct Slot {
    std::unique_ptr<Provider> provider;
    ProviderStatus status;
  };
  Clock::time_point now() const;
  void sleep(std::chrono::milliseconds d) const;
  void setState(Slot& s, std::string state, std::optional<Clock::time_point> until, std::string scope,
                std::string reason);
  bool capable(const Provider& p, const LlmRequest& req) const;
  bool refresh(Slot& s);
  void probe(Slot& s);
  LlmResponse attempt(Slot& s, const LlmRequest& req, StreamSink* sink, int& attempts);
  void captureFixture(const LlmError& e) const;

  GatewayConfig cfg_;
  std::vector<Slot> providers_;
  std::shared_ptr<StateStore> store_;
  std::shared_ptr<UsageSink> usage_;
  GatewayHooks hooks_;
  std::mutex mu_;  // guards provider states (calls themselves run unlocked)
};

// Process-wide gateway. initGateway() reads THELIFE_LLM_CONFIG (a JSON file, see config/llm.json) and
// THELIFE_LLM_BASE_URL / THELIFE_LLM_MODEL overrides; with neither, the gateway is unconfigured.
void initGateway();
Gateway& gateway();
// For tests and tools: replaces the process-wide gateway.
void setGateway(std::unique_ptr<Gateway> g);

// Storage key of a provider's state (name#hash(endpoint, model)).
std::string stateKey(const ProviderConfig& c);

// Parses a config document (exposed for tests). Throws std::invalid_argument on bad input.
GatewayConfig parseConfig(const Json::Value& doc);

}  // namespace thelife::llm
