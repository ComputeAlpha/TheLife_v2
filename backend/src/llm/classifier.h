#pragma once

#include <map>
#include <optional>
#include <string>

#include "llm/types.h"

namespace thelife::llm {

// What came back from one HTTP exchange with a provider.
struct HttpResult {
  int status = 0;                             // 0: no HTTP response (connect failure, timeout, reset)
  std::map<std::string, std::string> headers; // lower-cased names
  std::string body;
  std::string transportError;                 // set when status == 0
};

// Classifies a failed exchange (non-2xx or transport failure) into exactly one class (agent_lld.md §3.3).
// Error bodies are parsed as JSON whatever the Content-Type (Ollama sends JSON 404s as text/html).
// `now` anchors relative reset hints.
LlmError classify(const HttpResult& r, Clock::time_point now);

// A 2xx body that is still a failure: empty content without tool calls, or an unknown finish_reason
// (spikes/llm/REPORT.md, "bad_role": 200 with finish_reason "load"). Returns nullopt when the response is usable.
std::optional<LlmError> checkCompletion(const LlmResponse& r);

// RFC 3339 helpers (UTC, whole seconds).
std::string toRfc3339(Clock::time_point t);
std::optional<Clock::time_point> parseRfc3339(const std::string& s);

}  // namespace thelife::llm
