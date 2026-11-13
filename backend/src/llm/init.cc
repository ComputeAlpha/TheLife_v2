#include <drogon/drogon.h>

#include <fstream>
#include <sstream>

#include "core/env.h"
#include "core/json.h"
#include "db/llm_store.h"
#include "llm/gateway.h"

namespace thelife::llm {

void initGateway() {
  const auto path = env::get("THELIFE_LLM_CONFIG");
  const auto baseUrl = env::get("THELIFE_LLM_BASE_URL");
  Json::Value doc;
  if (!path.empty()) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("THELIFE_LLM_CONFIG: cannot read " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    try {
      doc = json::parse(ss.str());
    } catch (const std::exception& e) {
      throw std::runtime_error("THELIFE_LLM_CONFIG: " + std::string(e.what()));
    }
  } else if (!baseUrl.empty()) {
    // Minimal single-provider setup (tests, quick local runs).
    Json::Value p;
    p["name"] = "primary";
    p["base_url"] = baseUrl;
    p["api_key_env"] = "THELIFE_LLM_API_KEY";
    p["models"]["chat"] = env::get("THELIFE_LLM_MODEL", "glm-5.3-flash:cloud");
    p["caps"]["tools"] = true;
    p["caps"]["vision"] = true;
    p["caps"]["json_schema"] = false;
    doc["providers"].append(p);
  } else {
    LOG_INFO << "llm: no THELIFE_LLM_CONFIG or THELIFE_LLM_BASE_URL; the assistant runs in manual mode";
    return;
  }
  if (!path.empty() && !baseUrl.empty()) {  // point the primary at another endpoint (fake-llm in tests)
    for (auto& p : doc["providers"]) {
      if (p.get("kind", "openai").asString() == "openai") {
        p["base_url"] = baseUrl;
        break;
      }
    }
  }
  auto cfg = parseConfig(doc);
  cfg.fixtureDir = env::get("THELIFE_LLM_FIXTURE_DIR");
  if (const auto tz = env::get("THELIFE_LLM_RESET_TZ"); !tz.empty()) cfg.weeklyReset.timezone = tz;
  auto transport = std::shared_ptr<Transport>(makeCurlTransport());
  std::vector<std::unique_ptr<Provider>> providers;
  for (const auto& pc : cfg.providers) {
    providers.push_back(pc.kind == "stub" ? makeStubProvider(pc) : makeOpenAiProvider(pc, transport));
    LOG_INFO << "llm provider " << pc.name << " (" << pc.kind << ", " << pc.role << ")"
             << (pc.kind == "openai" ? " model=" + pc.chatModel : "");
  }
  setGateway(std::make_unique<Gateway>(std::move(cfg), std::move(providers), db::makeDbStateStore(),
                                       db::makeDbUsageSink()));
}

}  // namespace thelife::llm
