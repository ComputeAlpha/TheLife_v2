#include "web/search.h"

#include <drogon/drogon.h>

#include <fstream>
#include <sstream>

#include "core/blocking.h"
#include "core/env.h"
#include "core/errors.h"
#include "core/json.h"
#include "llm/transport.h"

namespace thelife::web {

namespace {
Config cfg;

trantor::ConcurrentTaskQueue& pool() {
  static trantor::ConcurrentTaskQueue p(2, "web");
  return p;
}

ApiError unavailable(const std::string& why) {
  return ApiError{503, "web-search-unavailable", "Web search unavailable", why};
}
}  // namespace

void init() {
  cfg = Config{};
  Json::Value section;
  if (const auto path = env::get("THELIFE_LLM_CONFIG"); !path.empty()) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
      section = json::parse(ss.str())["web_search"];
    } catch (const std::exception&) {
    }
  }
  if (section.isObject()) {
    cfg.baseUrl = section.get("base_url", "").asString();
    const auto keyEnv = section.get("api_key_env", "OLLAMA_API_KEY").asString();
    if (!keyEnv.empty()) cfg.apiKey = env::get(keyEnv);
    cfg.timeoutSec = section.get("timeout_s", 15).asInt();
    cfg.dailyLimit = section.get("daily_limit", 50).asInt();
  }
  if (!env::get("THELIFE_WEB_DAILY_LIMIT").empty()) cfg.dailyLimit = env::getInt("THELIFE_WEB_DAILY_LIMIT", 50, 0, 100000);
  const auto override = env::get("THELIFE_WEB_SEARCH_URL");
  if (!override.empty()) cfg.baseUrl = override;
  while (!cfg.baseUrl.empty() && cfg.baseUrl.back() == '/') cfg.baseUrl.pop_back();
  // ollama.com needs a key; a test endpoint given through THELIFE_WEB_SEARCH_URL does not.
  cfg.enabled = !cfg.baseUrl.empty() && (!cfg.apiKey.empty() || !override.empty());
  if (cfg.enabled) {
    LOG_INFO << "web search: " << cfg.baseUrl << " (daily limit " << cfg.dailyLimit << ")";
  } else {
    LOG_INFO << "web search: off (set OLLAMA_API_KEY to enable)";
  }
}

const Config& config() { return cfg; }

std::string clean(const std::string& text, std::size_t maxBytes) {
  std::string out;
  out.reserve(std::min(text.size(), maxBytes));
  for (unsigned char c : text) {
    if (c < 0x20 && c != '\n' && c != '\t') continue;
    if (c == 0x7F) continue;
    out.push_back(static_cast<char>(c));
  }
  if (out.size() > maxBytes) {
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;  // not inside a code point
    out = out.substr(0, cut) + "…";
  }
  return out;
}

drogon::Task<Json::Value> call(std::string endpoint, Json::Value body) {
  if (!cfg.enabled) throw unavailable("Web search is not configured on this server.");
  const auto url = cfg.baseUrl + "/" + endpoint;
  const auto payload = json::compact(body);
  const auto r = co_await runBlockingOn(pool(), [url, payload] {
    static const auto transport = llm::makeCurlTransport();
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}, {"Accept", "application/json"}};
    if (!cfg.apiKey.empty()) headers["Authorization"] = "Bearer " + cfg.apiKey;
    return transport->post(url, headers, payload, cfg.timeoutSec, cfg.timeoutSec, {});
  });
  if (r.status == 0) throw unavailable("The search service could not be reached.");
  if (r.status == 401 || r.status == 403) throw unavailable("The search service rejected the API key.");
  if (r.status == 429) throw unavailable("The search service is rate limiting; try again later.");
  if (r.status < 200 || r.status >= 300) {
    throw unavailable("The search service answered " + std::to_string(r.status) + ".");
  }
  try {
    co_return json::parse(r.body);
  } catch (const std::exception&) {
  }
  throw unavailable("The search service sent an unreadable answer.");
}

}  // namespace thelife::web
