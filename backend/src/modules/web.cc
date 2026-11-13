// web.search / web.fetch for the agent (Ollama web search). Results are untrusted: the agent loop marks the turn,
// and the policy then holds every write of that turn for approval (prompt injection through web content).
#include "core/errors.h"
#include "modules/modules.h"
#include "web/search.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

// Daily cap per user over search and fetch, counted from the audit trail.
Task<void> checkDailyLimit(ToolCall& c) {
  const auto r = co_await c.tx.exec(
      "SELECT count(*)::int AS n FROM agent_actions WHERE user_id = $1::uuid AND tool IN ('web.search', 'web.fetch') "
      "AND outcome = 'executed' AND created_at > now() - interval '24 hours'",
      c.principal.userId);
  if (r[0]["n"].as<int>() >= web::config().dailyLimit) {
    throw ApiError{429, "web-search-limit", "Web search limit reached",
                   "The daily web search limit for this account is used up."};
  }
}

Task<Json::Value> search(ToolCall& c) {
  co_await checkDailyLimit(c);
  Json::Value body;
  body["query"] = c.args["query"].asString();
  body["max_results"] = c.args.get("max_results", 5).asInt();
  const auto res = co_await web::call("web_search", body);
  Json::Value out;
  out["untrusted"] = true;
  out["results"] = Json::Value(Json::arrayValue);
  for (const auto& r : res["results"]) {
    if (out["results"].size() >= 10) break;
    Json::Value item;
    item["title"] = web::clean(r["title"].asString(), 300);
    item["url"] = web::clean(r["url"].asString(), 2000);
    item["content"] = web::clean(r["content"].asString(), 1200);
    out["results"].append(item);
  }
  co_return out;
}

Task<Json::Value> fetch(ToolCall& c) {
  co_await checkDailyLimit(c);
  Json::Value body;
  body["url"] = c.args["url"].asString();
  const auto res = co_await web::call("web_fetch", body);
  Json::Value out;
  out["untrusted"] = true;
  out["url"] = c.args["url"].asString();
  out["title"] = web::clean(res["title"].asString(), 300);
  out["content"] = web::clean(res["content"].asString(), 6000);
  out["links"] = Json::Value(Json::arrayValue);
  for (const auto& l : res["links"]) {
    if (out["links"].size() >= 20) break;
    if (l.isString()) out["links"].append(web::clean(l.asString(), 500));
  }
  co_return out;
}

}  // namespace

void bindWeb(tools::ToolRegistry& reg) {
  reg.bind("web.search", search);
  reg.bind("web.fetch", fetch);
}

}  // namespace thelife::modules
