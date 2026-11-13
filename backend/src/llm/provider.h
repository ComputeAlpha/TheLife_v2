#pragma once

#include <json/value.h>

#include <memory>
#include <string>

#include "llm/transport.h"
#include "llm/types.h"

namespace thelife::llm {

struct ProviderConfig {
  std::string name;
  std::string kind = "openai";  // openai | stub
  std::string role = "primary"; // primary | fallback
  std::string baseUrl;          // ends with /v1
  std::string apiKey;           // resolved from api_key_env; empty for local Ollama
  std::string chatModel, visionModel;
  Capabilities caps;
  int timeoutSec = 180;
  int idleTimeoutSec = 60;
};

// One provider endpoint. complete() makes exactly one attempt and throws a classified LlmError on failure;
// retries, routing and state live in the Gateway.
class Provider {
 public:
  virtual ~Provider() = default;
  virtual const ProviderConfig& config() const = 0;
  virtual LlmResponse complete(const LlmRequest& req, StreamSink* sink) = 0;
};

std::unique_ptr<Provider> makeOpenAiProvider(ProviderConfig cfg, std::shared_ptr<Transport> transport);
// Placeholder fallback (agent_lld.md §3.1): reports no capabilities and refuses every call.
std::unique_ptr<Provider> makeStubProvider(ProviderConfig cfg);

// Exposed for tests.
Json::Value buildChatBody(const LlmRequest& req, const std::string& model, bool stream);
LlmResponse parseChatResponse(const Json::Value& body);

// Separates reasoning that leaked into content (glm-5.3-flash with tool calls: "...reasoning</think>answer").
struct SplitContent {
  std::string reasoning, content;
};
SplitContent splitThinking(const std::string& content);

// Incremental parser for an OpenAI chat-completions SSE stream.
class SseAccumulator {
 public:
  explicit SseAccumulator(StreamSink* sink) : sink_(sink) {}
  void feed(std::string_view bytes);
  // Finishes parsing; throws LlmError if the stream carried an error event or never finished.
  LlmResponse finish();

 private:
  void line(std::string_view l);
  void event(const std::string& data);
  StreamSink* sink_;
  std::string buf_, data_;
  LlmResponse out_;
  std::vector<ToolCallOut> calls_;
  bool done_ = false;
  bool thinkClosed_ = false;
  std::string streamError_;
};

}  // namespace thelife::llm
