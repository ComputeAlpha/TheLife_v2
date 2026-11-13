#pragma once

#include <json/value.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// LLM gateway types (agent_lld.md §3). Everything that talks to a model goes through llm::Gateway.
namespace thelife::llm {

using Clock = std::chrono::system_clock;

// Orders the catch-up queue after an outage; also decides whether a short rate limit is waited out.
enum class Priority { Interactive, Proactive, Vision, Background };
const char* toString(Priority p);

struct ToolCallOut {
  std::string id;
  std::string name;
  std::string arguments;  // JSON text exactly as the model produced it
};

struct Message {
  std::string role;                        // system | user | assistant | tool
  std::string content;
  std::vector<std::string> imageDataUris;  // user messages only (data:image/jpeg;base64,...)
  std::vector<ToolCallOut> toolCalls;      // assistant messages
  std::string toolCallId;                  // tool messages

  static Message system(std::string text) { return {"system", std::move(text), {}, {}, {}}; }
  static Message user(std::string text) { return {"user", std::move(text), {}, {}, {}}; }
  static Message assistant(std::string text, std::vector<ToolCallOut> calls = {}) {
    return {"assistant", std::move(text), {}, std::move(calls), {}};
  }
  static Message tool(std::string callId, std::string result) {
    return {"tool", std::move(result), {}, {}, std::move(callId)};
  }
};

struct ToolSpec {
  std::string name;
  std::string description;
  Json::Value parameters;  // JSON Schema object
};

struct LlmRequest {
  std::string purpose = "agent.turn";  // "agent.turn", "vision.analyze", "memory.compress_day", ...
  Priority priority = Priority::Interactive;
  std::string tenantId;                // required: every call is accounted to a tenant
  std::optional<std::string> conversationId;
  std::vector<Message> messages;
  std::vector<ToolSpec> tools;
  std::string toolChoice = "auto";     // auto | none | required
  bool vision = false;                 // use the provider's vision model
  int maxTokens = 2048;
  std::optional<double> temperature;
  // Never "none" (it leaks reasoning into content on glm-5.3-flash, spikes/llm/REPORT.md); "low" for jobs.
  std::optional<std::string> reasoningEffort;
  std::string promptId, promptVersion;
};

struct Usage {
  int tokensIn = 0;
  int tokensOut = 0;
};

struct LlmResponse {
  std::string content;
  std::vector<ToolCallOut> toolCalls;
  std::string finishReason;
  Usage usage;
  std::string reasoning;  // kept only for debug capture, never stored as message content
  std::string provider;
  std::string model;
  int attempts = 1;
};

// Receives content deltas while a response streams. reset() means "discard what you showed": the gateway is
// retrying after a failure mid-stream. Called from the gateway's worker thread.
class StreamSink {
 public:
  virtual ~StreamSink() = default;
  virtual void delta(std::string_view text) = 0;
  virtual void reset() {}
};

// Error taxonomy (§3.3). Every provider failure maps to exactly one class.
enum class ErrorClass {
  Transient,
  RateLimited,
  Unclassified,
  QuotaExhausted,
  ContextTooLong,
  AuthFailed,
  BadRequest,
  InvalidOutput,
  ContentFiltered,
  ProviderUnavailable,  // no provider can take the call (all exhausted, disabled, or not configured)
};
const char* toString(ErrorClass c);

class LlmError : public std::runtime_error {
 public:
  LlmError(ErrorClass c, std::string what, int httpStatus = 0)
      : std::runtime_error(std::string(toString(c)) + ": " + what), cls(c), status(httpStatus),
        detail(std::move(what)) {}
  ErrorClass cls;
  int status;
  std::string detail;                        // safe to log: never contains request or response content
  std::optional<int> retryAfterSec;
  std::string quotaScope;                    // hourly | session | daily | weekly | unknown (QuotaExhausted)
  std::optional<Clock::time_point> resetsAt; // QuotaExhausted, ProviderUnavailable while exhausted
  std::string rawBody;  // Unclassified only, bounded: for fixture-capture mode, never logged
};

struct Capabilities {
  bool tools = true;
  bool jsonSchema = false;  // native response_format json_schema (false on glm-5.3-flash)
  bool vision = false;
  int ctxTokens = 32768;
};

}  // namespace thelife::llm
