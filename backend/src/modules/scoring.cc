// Scoring configuration and scores (scoring_lld.md §3, §5, §11).
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "crud/resource.h"
#include "db/job_store.h"
#include "modules/modules.h"
#include "scoring/engine.h"
#include "scoring/inputs.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

std::vector<std::string> scoreProjection() { return crud::projectionFor(tools::registry(), "scores"); }

Task<Json::Value> history(ToolCall& c) {
  const auto rows = co_await c.tx.exec(
      "SELECT to_jsonb(v)::text AS j FROM score_versions v JOIN score_versions s "
      "  ON s.user_id = v.user_id AND s.period = v.period AND s.period_start = v.period_start "
      "WHERE s.id = $1::uuid ORDER BY v.created_at DESC, v.id DESC",
      c.args["id"].asString());
  if (rows.empty()) throw ApiError::notFound();
  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  const auto proj = scoreProjection();
  for (const auto& r : rows) out["items"].append(crud::toApi(json::parse(r["j"].as<std::string>()), proj));
  co_return out;
}

// The user disputes the adjustment (§8): it is re-run once with the reason; the outcome is kept for the
// dispute statistics.
Task<Json::Value> dispute(ToolCall& c) {
  const auto id = c.args["id"].asString();
  const auto s = co_await c.tx.exec(
      "SELECT user_id::text AS u, to_char(period_start, 'YYYY-MM-DD') AS d, period, status, adjustment_pct IS NOT NULL AS adj "
      "FROM scores WHERE id = $1::uuid AND user_id = $2::uuid",
      id, c.principal.userId);
  if (s.empty()) throw ApiError::notFound("Not found, or not the current version of this score.");
  if (s[0]["period"].as<std::string>() != "day" || !s[0]["adj"].as<bool>()) {
    throw ApiError::conflict("Only a day score with an assistant adjustment can be disputed.");
  }
  const auto open = co_await c.tx.exec("SELECT 1 FROM score_disputes WHERE score_id = $1::uuid AND status = 'queued'", id);
  if (!open.empty()) throw ApiError::conflict("This score already has a dispute in progress.");
  const auto disputeId = ids::newUuidV7();
  co_await c.tx.exec(
      "INSERT INTO score_disputes (id, tenant_id, user_id, score_id, reason) VALUES ($1::uuid, $2::uuid, $3::uuid, "
      "$4::uuid, $5)",
      disputeId, c.principal.tenantId, c.principal.userId, id, c.args["reason"].asString());
  db::jobs::NewJob j;
  j.kind = "scores.close_day";
  j.payload["user_id"] = c.principal.userId;
  j.payload["local_day"] = s[0]["d"].as<std::string>();
  j.payload["force_adjust"] = true;
  j.payload["dispute_id"] = disputeId;
  j.payload["reason"] = c.args["reason"].asString();
  j.dedupeKey = "dispute:" + disputeId;
  j.priority = 1;
  co_await db::jobs::enqueue(c.tx, j);
  Json::Value out;
  out["status"] = "queued";
  co_return out;
}

Task<Json::Value> todayProgress(ToolCall& c) {
  const auto r = co_await c.tx.exec(
      "SELECT to_char(((now() AT TIME ZONE timezone) - make_interval(hours => day_start_hour))::date, 'YYYY-MM-DD') AS d, "
      "  extract(epoch FROM now())::bigint AS now FROM users WHERE id = $1::uuid",
      c.principal.userId);
  const auto in = co_await scoring::gatherDay(c.tx, c.principal.userId, r[0]["d"].as<std::string>());
  co_return scoring::progress(in, r[0]["now"].as<int64_t>());
}

Task<Json::Value> preemptions(ToolCall& c) {
  std::vector<std::optional<std::string>> params{
      c.principal.userId, c.args.isMember("from") ? std::optional(c.args["from"].asString()) : std::nullopt,
      c.args.isMember("to") ? std::optional(c.args["to"].asString()) : std::nullopt};
  const auto rows = co_await c.tx.execParams(
      "SELECT jsonb_build_object('local_day', local_day, 'kind', kind, 'displaced_type', displaced_type, "
      "  'displaced_id', displaced_id, 'displaced_title', displaced_title, 'minutes', minutes)::text AS j "
      "FROM preemptions WHERE user_id = $1::uuid AND local_day >= coalesce($2::date, current_date - 30) "
      "  AND local_day <= coalesce($3::date, current_date) ORDER BY local_day, kind LIMIT 500",
      std::move(params));
  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  for (const auto& r : rows) out["items"].append(json::parse(r["j"].as<std::string>()));
  co_return out;
}

Json::Value settingsJson(const scoring::Settings& s) {
  Json::Value out;
  for (const auto& [k, v] : s.weights) out["weights"][k] = v;
  for (const auto& [k, v] : s.alpha) out["plan_share"][k] = v;
  return out;
}

Task<Json::Value> getSettings(ToolCall& c) {
  co_return settingsJson(co_await scoring::loadSettings(c.tx, c.principal.userId));
}

Task<Json::Value> updateSettings(ToolCall& c) {
  // The body's minProperties does not survive the merge into tool arguments; enforce it here.
  if (!c.args.isMember("weights") && !c.args.isMember("plan_share")) {
    throw ApiError::validation({{"", "give weights or plan_share"}});
  }
  auto s = co_await scoring::loadSettings(c.tx, c.principal.userId);
  for (const auto& k : c.args["weights"].getMemberNames()) s.weights[k] = c.args["weights"][k].asDouble();
  for (const auto& k : c.args["plan_share"].getMemberNames()) s.alpha[k] = c.args["plan_share"][k].asDouble();
  double total = 0;
  for (const auto& [_, w] : s.weights) total += w;
  if (total <= 0) throw ApiError::validation({{"/weights", "at least one area needs a weight above 0"}});
  const auto j = settingsJson(s);
  co_await c.tx.exec(
      "INSERT INTO scoring_settings (tenant_id, user_id, weights, plan_share) VALUES ($1::uuid, $2::uuid, $3::jsonb, "
      "$4::jsonb) ON CONFLICT (user_id) DO UPDATE SET weights = EXCLUDED.weights, plan_share = EXCLUDED.plan_share, "
      "updated_at = now()",
      c.principal.tenantId, c.principal.userId, json::compact(j["weights"]), json::compact(j["plan_share"]));
  co_return j;
}

void validateTarget(Json::Value& f, bool creating) {
  if (f.isMember("match") && f["match"].isObject()) {
    const auto& m = f["match"];
    if (m["link_type"].isString() != m["link_id"].isString()) {
      throw ApiError::validation({{"/match", "link_type and link_id go together"}});
    }
  }
  (void)creating;
}

}  // namespace

void bindScoring(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "targets", .table = "targets", .filters = {}, .validate = validateTarget});
  crud::bindCrud(reg, {.prefix = "day_profiles", .table = "day_profiles", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "day_profile_overrides",
                       .table = "day_profile_overrides",
                       .filters = {{"from", "r.local_day >= $?::date"}, {"to", "r.local_day <= $?::date"}},
                       .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "work_contexts", .table = "work_contexts", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "scores",
                       .table = "scores",
                       .filters = {{"from", "r.period_start >= $?::date"}, {"to", "r.period_start <= $?::date"}},
                       .validate = nullptr});
  reg.bind("scores.history", history);
  reg.bind("scores.dispute", dispute);
  reg.bind("scores.today_progress", todayProgress);
  reg.bind("scores.preemptions", preemptions);
  reg.bind("scores.settings", getSettings);
  reg.bind("scores.update_settings", updateSettings);
}

}  // namespace thelife::modules
