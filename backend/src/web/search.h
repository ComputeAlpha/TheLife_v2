#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <string>

// Ollama web search and web fetch (https://docs.ollama.com/capabilities/web-search): POST <base>/web_search
// {query, max_results ≤ 10} → {results[{title, url, content}]}, POST <base>/web_fetch {url} → {title, content,
// links[]}, both with Authorization: Bearer $OLLAMA_API_KEY.
namespace thelife::web {

struct Config {
  bool enabled = false;
  std::string baseUrl;  // https://ollama.com/api
  std::string apiKey;
  int timeoutSec = 15;
  int dailyLimit = 50;  // web calls per user per 24 h (search + fetch)
};

// Reads the "web_search" section of THELIFE_LLM_CONFIG; THELIFE_WEB_SEARCH_URL overrides the endpoint (tests,
// where no key is needed). Without a key for ollama.com the tools answer "not configured".
void init();
const Config& config();

// One call; throws ApiError 503 when unconfigured, unreachable, rejected or rate limited (the agent sees the
// reason as the tool result). Runs on a small dedicated pool, never on an event loop thread.
drogon::Task<Json::Value> call(std::string endpoint, Json::Value body);

// Text from the web made safe to put in a prompt: control characters removed, cut on a UTF-8 boundary.
std::string clean(const std::string& text, std::size_t maxBytes);

}  // namespace thelife::web
