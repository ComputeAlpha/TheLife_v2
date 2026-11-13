#include "http/llm_routes.h"

#include "db/tenant_tx.h"
#include "http/route.h"
#include "llm/classifier.h"
#include "llm/gateway.h"

namespace thelife::http {

namespace {

drogon::Task<drogon::HttpResponsePtr> llmStatus(RequestContext& ctx) {
  auto& gw = llm::gateway();
  std::optional<llm::Clock::time_point> resetsAt;
  Json::Value body;
  body["mode"] = gw.mode(&resetsAt);
  body["resets_at"] = resetsAt ? Json::Value(llm::toRfc3339(*resetsAt)) : Json::Value(Json::nullValue);
  Json::Value providers(Json::arrayValue);
  const auto statuses = gw.status();
  for (std::size_t i = 0; i < statuses.size() && i < gw.config().providers.size(); ++i) {
    const auto& s = statuses[i];
    const auto& pc = gw.config().providers[i];
    Json::Value p;
    p["name"] = s.provider;
    p["role"] = pc.role;
    p["kind"] = pc.kind;
    p["model"] = pc.kind == "stub" ? Json::Value(Json::nullValue) : Json::Value(pc.chatModel);
    p["state"] = s.state;
    p["until"] = s.until ? Json::Value(llm::toRfc3339(*s.until)) : Json::Value(Json::nullValue);
    p["quota_scope"] = s.quotaScope.empty() ? Json::Value(Json::nullValue) : Json::Value(s.quotaScope);
    providers.append(p);
  }
  body["providers"] = providers;

  // This week's usage for the caller's tenant (Monday 00:00 in the user's timezone onward), §3.5.
  auto tx = co_await db::TenantTx::begin(ctx.who());
  const auto r = co_await tx.exec(
      "SELECT to_char(w.start, 'YYYY-MM-DD') AS week_start, coalesce(sum(u.requests), 0)::bigint AS requests, "
      "  coalesce(sum(u.failures), 0)::bigint AS failures, coalesce(sum(u.tokens_in), 0)::bigint AS tokens_in, "
      "  coalesce(sum(u.tokens_out), 0)::bigint AS tokens_out, coalesce(sum(u.images), 0)::bigint AS images "
      "FROM (SELECT date_trunc('week', now() AT TIME ZONE timezone)::date AS start FROM users WHERE id = $1::uuid) w "
      "LEFT JOIN llm_usage u ON u.day >= w.start GROUP BY w.start",
      ctx.who().userId);
  co_await tx.commit();
  Json::Value usage;
  usage["weekly_request_estimate"] = gw.config().weeklyRequestEstimate;
  if (!r.empty()) {
    usage["week_start"] = r[0]["week_start"].as<std::string>();
    for (const char* k : {"requests", "failures", "tokens_in", "tokens_out", "images"}) {
      usage[k] = static_cast<Json::Int64>(r[0][k].as<long long>());
    }
  }
  body["usage"] = usage;
  co_return jsonResponse(body);
}

}  // namespace

void registerLlmRoutes() { addRoute("/api/v1/system/llm-status", drogon::Get, Access::Authenticated, llmStatus); }

}  // namespace thelife::http
