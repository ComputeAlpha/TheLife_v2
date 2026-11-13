// Score jobs (scoring_lld.md §2, §8–§10).
//   scores.close_day     day score: deterministic base, then one LLM adjustment (±30 %); idempotent per inputs hash;
//                        re-runs reuse the adjustment unless the base moved > 5 points or the user disputed it
//   scores.close_period  week and month scores from the day scores, with coverage and weekly targets
#include "scoring/jobs.h"

#include <drogon/drogon.h>

#include <cmath>

#include "agent/prompt.h"
#include "core/crypto.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/job_store.h"
#include "jobs/worker.h"
#include "llm/gateway.h"
#include "scoring/engine.h"
#include "scoring/inputs.h"

namespace thelife::scoring {

namespace {

using drogon::Task;

double round1(double v) { return std::round(v * 10.0) / 10.0; }

const Json::Value& adjustSchema() {
  static const Json::Value s = json::parse(R"({
    "type": "object", "additionalProperties": false, "required": ["adjustment_pct", "reasons", "goal_alignment"],
    "properties": {
      "adjustment_pct": {"type": "number", "minimum": -30, "maximum": 30},
      "reasons": {"type": "array", "maxItems": 6, "items": {
        "type": "object", "additionalProperties": false, "required": ["text", "direction"],
        "properties": {
          "text": {"type": "string", "minLength": 1, "maxLength": 300},
          "direction": {"type": "string", "enum": ["up", "down"]},
          "refs": {"type": "array", "maxItems": 5, "items": {"type": "object", "required": ["type", "id"],
            "properties": {"type": {"type": "string", "maxLength": 40}, "id": {"type": "string", "maxLength": 64}}}}}}},
      "goal_alignment": {"type": "string", "enum": ["low", "medium", "high"]}},
    "if": {"properties": {"adjustment_pct": {"const": 0}}},
    "then": {},
    "else": {"properties": {"reasons": {"minItems": 1}}}})");
  return s;
}

// What the adjustment sees: the breakdown (compact), preemptions, active goals and plans, the day's summaries, the
// work contexts and a dispute reason if any.
Task<Json::Value> adjustmentInput(db::TenantTx& tx, const std::string& user, const DayInputs& in, const DayResult& r,
                                  const std::string& disputeReason) {
  Json::Value v;
  v["day"] = in.localDay;
  v["profile"] = in.profile;
  v["base"] = *r.base;
  v["areas"] = r.breakdown["areas"];
  v["consistency"] = r.consistency;
  for (const auto& p : r.preemptions) {
    Json::Value j;
    j["kind"] = p.kind, j["displaced"] = p.displacedTitle, j["type"] = p.displacedType, j["id"] = p.displacedId;
    j["minutes"] = p.minutes;
    v["preemptions"].append(j);
  }
  for (const auto& g : co_await tx.exec(
           "SELECT id::text AS id, title, area::text AS area, horizon, coalesce(to_char(due_on, 'YYYY-MM-DD'), '') AS due "
           "FROM goals WHERE deleted_at IS NULL AND status = 'active' ORDER BY created_at LIMIT 20")) {
    Json::Value j;
    j["id"] = g["id"].as<std::string>(), j["title"] = g["title"].as<std::string>(), j["area"] = g["area"].as<std::string>();
    j["horizon"] = g["horizon"].as<std::string>(), j["due_on"] = g["due"].as<std::string>();
    v["goals"].append(j);
  }
  for (const auto& p : co_await tx.exec(
           "SELECT id::text AS id, title FROM plans WHERE deleted_at IS NULL AND status = 'active' "
           "  AND period_start <= $1::date AND period_end >= $1::date LIMIT 10",
           in.localDay)) {
    Json::Value j;
    j["id"] = p["id"].as<std::string>(), j["title"] = p["title"].as<std::string>();
    v["plans"].append(j);
  }
  for (const auto& s : co_await tx.exec(
           "SELECT channel, content FROM episodic_summaries WHERE user_id = $1::uuid AND period = 'day' "
           "  AND period_start = $2::date",
           user, in.localDay)) {
    v["day_summary"][s["channel"].as<std::string>()] = s["content"].as<std::string>();
  }
  for (const auto& c : co_await tx.exec("SELECT name, evidence_mode FROM work_contexts WHERE deleted_at IS NULL")) {
    Json::Value j;
    j["name"] = c["name"].as<std::string>(), j["evidence"] = c["evidence_mode"].as<std::string>();
    v["work_contexts"].append(j);
  }
  // What actually happened, planned or not: an unplanned day of goal-critical work is what the adjustment is for.
  v["evidence_minutes"] = r.breakdown["evidence_minutes"];
  for (const auto& a : co_await tx.exec(
           "SELECT id::text AS id, title, area::text AS area, "
           "  (extract(epoch FROM (least(ended_at, to_timestamp($2::bigint)) - greatest(started_at, to_timestamp($1::bigint)))) / 60)::int AS m "
           "FROM activities WHERE deleted_at IS NULL AND started_at < to_timestamp($2::bigint) "
           "  AND ended_at > to_timestamp($1::bigint) ORDER BY started_at LIMIT 40",
           std::to_string(in.dayFrom), std::to_string(in.dayTo))) {
    Json::Value j;
    j["id"] = a["id"].as<std::string>(), j["title"] = a["title"].as<std::string>(), j["area"] = a["area"].as<std::string>();
    j["minutes"] = a["m"].as<int>();
    v["logged_activities"].append(j);
  }
  if (!disputeReason.empty()) v["dispute_from_user"] = disputeReason;
  co_return v;
}

struct Adjustment {
  double pct = 0;
  Json::Value detail;
};

// One structured call. Throws LlmError (callers decide between provisional and retry).
Task<Adjustment> adjust(const std::string& tenant, const std::string& userName, const Json::Value& input) {
  const auto p = agent::prompt("jobs/score_adjust");
  llm::LlmRequest req;
  req.tenantId = tenant;
  req.purpose = "scores.adjust";
  req.priority = llm::Priority::Background;
  req.reasoningEffort = "low";
  req.promptId = p.id;
  req.promptVersion = p.version + "+" + p.hash;
  req.maxTokens = 2000;
  std::string text = json::compact(input);
  if (text.size() > 40000) text = text.substr(0, 40000);
  req.messages = {llm::Message::system(p.render({{"user_name", userName}})), llm::Message::user(text)};
  auto out = co_await llm::gateway().structured(std::move(req), "score_adjustment", adjustSchema());
  Adjustment a;
  a.pct = round1(std::clamp(out["adjustment_pct"].asDouble(), -30.0, 30.0));
  if (a.pct != 0 && out["reasons"].empty()) {  // also enforced by the schema's if/else
    throw llm::LlmError(llm::ErrorClass::InvalidOutput, "adjustment without reasons", 200);
  }
  a.detail = out;
  a.detail["adjustment_pct"] = a.pct;
  co_return a;
}

Json::Value preemptionsJson(const DayResult& r) {
  Json::Value arr(Json::arrayValue);
  for (const auto& p : r.preemptions) {
    Json::Value j;
    j["kind"] = p.kind, j["type"] = p.displacedType, j["id"] = p.displacedId, j["title"] = p.displacedTitle;
    j["minutes"] = p.minutes;
    arr.append(j);
  }
  return arr;
}

struct NewVersion {
  std::string user, period, start, profile, hash, status;
  Json::Value breakdown, consistency, coverage, adjustment;
  std::optional<double> base, pct, final;
};

Task<std::string> insertVersion(db::TenantTx& tx, const NewVersion& v) {
  co_await tx.exec(
      "UPDATE score_versions SET is_current = false WHERE user_id = $1::uuid AND period = $2 AND period_start = $3::date "
      "AND is_current",
      v.user, v.period, v.start);
  const auto id = ids::newUuidV7();
  auto num = [](const std::optional<double>& d) {
    return d ? std::optional<std::string>(std::to_string(*d)) : std::nullopt;
  };
  std::vector<std::optional<std::string>> params{
      id, tx.tenantId(), v.user, v.period, v.start, v.profile.empty() ? std::nullopt : std::optional(v.profile),
      kFormulaVersion, v.hash, json::compact(v.breakdown), json::compact(v.consistency),
      v.coverage.isNull() ? std::nullopt : std::optional(json::compact(v.coverage)), num(v.base), num(v.pct),
      v.adjustment.isNull() ? std::nullopt : std::optional(json::compact(v.adjustment)), num(v.final), v.status};
  co_await tx.execParams(
      "INSERT INTO score_versions (id, tenant_id, user_id, period, period_start, profile, formula_version, inputs_hash, "
      "breakdown, consistency, coverage, base, adjustment_pct, adjustment, final, status) VALUES ($1::uuid, $2::uuid, "
      "$3::uuid, $4, $5::date, $6, $7, $8, $9::jsonb, $10::jsonb, $11::jsonb, round($12::numeric, 2), "
      "round($13::numeric, 1), $14::jsonb, round($15::numeric, 2), $16)",
      std::move(params));
  co_return id;
}

Task<void> closeDay(const jobs::Job& job) {
  const auto user = job.payload["user_id"].asString();
  const auto day = job.payload["local_day"].asString();
  const bool force = job.payload.get("force_adjust", false).asBool();
  const auto disputeId = job.payload.get("dispute_id", "").asString();
  const auto reason = job.payload.get("reason", "").asString();

  DayInputs in;
  DayResult r;
  Json::Value llmInput;
  std::string userName;
  struct Current {
    std::string hash, status;
    std::optional<double> base, pct;
    Json::Value adjustment;
  };
  std::optional<Current> current;
  {
    auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
    in = co_await gatherDay(tx, user, day);
    r = scoreDay(in);
    const auto cur = co_await tx.exec(
        "SELECT inputs_hash, status, base::float8 AS base, adjustment_pct::float8 AS pct, coalesce(adjustment::text, '') AS adj "
        "FROM score_versions WHERE user_id = $1::uuid AND period = 'day' AND period_start = $2::date AND is_current",
        user, day);
    if (!cur.empty()) {
      Current c;
      c.hash = cur[0]["inputs_hash"].as<std::string>();
      c.status = cur[0]["status"].as<std::string>();
      if (!cur[0]["base"].isNull()) c.base = cur[0]["base"].as<double>();
      if (!cur[0]["pct"].isNull()) c.pct = cur[0]["pct"].as<double>();
      if (!cur[0]["adj"].as<std::string>().empty()) c.adjustment = json::parse(cur[0]["adj"].as<std::string>());
      current = c;
    }
    // Unchanged inputs and nothing pending: no new version (§12 property: recompute is idempotent).
    if (current && current->hash == r.inputsHash && !force && current->status != "provisional") {
      co_await tx.commit();
      co_return;
    }
    if (r.base) {
      const auto u = co_await tx.exec("SELECT display_name FROM users WHERE id = $1::uuid", user);
      userName = u.empty() ? "the user" : u[0]["display_name"].as<std::string>();
      llmInput = co_await adjustmentInput(tx, user, in, r, reason);
    }
    co_await tx.commit();
  }

  NewVersion v;
  v.user = user, v.period = "day", v.start = day, v.profile = in.profile, v.hash = r.inputsHash;
  v.breakdown = r.breakdown, v.consistency = r.consistency, v.base = r.base;
  v.breakdown["preemptions"] = preemptionsJson(r);
  v.status = r.status;
  std::optional<llm::Clock::time_point> adjustLater;
  if (r.base) {
    const bool reuse = current && current->pct && !force && current->base && std::abs(*current->base - *r.base) <= 5.0;
    if (reuse) {
      v.pct = current->pct;
      v.adjustment = current->adjustment;
      v.status = "final";
    } else {
      std::optional<llm::LlmError> err;
      Adjustment a;
      try {
        a = co_await adjust(job.tenantId, userName, llmInput);
      } catch (const llm::LlmError& e) {
        err = e;
      }
      if (!err) {
        v.pct = a.pct;
        v.adjustment = a.detail;
        v.status = "final";
      } else if (err->cls == llm::ErrorClass::ProviderUnavailable) {
        v.status = "provisional";  // base only; the adjustment runs after the reset (§2, agent_lld.md §3.4)
        adjustLater = err->resetsAt.value_or(llm::Clock::now() + std::chrono::hours(1));
      } else if (err->cls == llm::ErrorClass::RateLimited) {
        throw jobs::Defer(llm::Clock::now() + std::chrono::seconds(err->retryAfterSec.value_or(60)), "rate limited");
      } else {
        throw std::runtime_error(std::string("score adjustment: ") + llm::toString(err->cls));
      }
    }
    if (v.pct) v.final = finalScore(*r.base, *v.pct);
  }

  auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
  co_await insertVersion(tx, v);
  co_await tx.exec("DELETE FROM preemptions WHERE user_id = $1::uuid AND local_day = $2::date", user, day);
  for (const auto& p : r.preemptions) {
    co_await tx.exec(
        "INSERT INTO preemptions (id, tenant_id, user_id, local_day, kind, displaced_type, displaced_id, "
        "displaced_title, minutes) VALUES ($1::uuid, $2::uuid, $3::uuid, $4::date, $5, $6, $7::uuid, $8, $9::int)",
        ids::newUuidV7(), job.tenantId, user, day, p.kind, p.displacedType, p.displacedId, p.displacedTitle,
        std::to_string(p.minutes));
  }
  if (!disputeId.empty()) {
    std::vector<std::optional<std::string>> params{
        disputeId, current && current->pct ? std::optional(std::to_string(*current->pct)) : std::nullopt,
        v.pct ? std::optional(std::to_string(*v.pct)) : std::nullopt};
    co_await tx.execParams(
        "UPDATE score_disputes SET status = 'done', before_pct = $2::numeric, after_pct = $3::numeric, "
        "resolved_at = now() WHERE id = $1::uuid",
        std::move(params));
  }
  if (adjustLater) {
    db::jobs::NewJob j;
    j.kind = "scores.close_day";
    j.payload = job.payload;
    j.payload["force_adjust"] = true;
    j.dedupeKey = "adjust:" + user + ":" + day;
    j.delaySec = static_cast<int>(std::max<long long>(
        1, std::chrono::duration_cast<std::chrono::seconds>(*adjustLater - llm::Clock::now()).count()));
    j.priority = 3;
    co_await db::jobs::enqueue(tx, j);
  }
  co_await tx.commit();
}

Task<void> closePeriod(const jobs::Job& job) {
  const auto user = job.payload["user_id"].asString();
  const auto period = job.payload["period"].asString();
  const auto start = job.payload["period_start"].asString();
  auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
  const std::string endExpr =
      period == "week" ? "$2::date + 6" : "($2::date + interval '1 month' - interval '1 day')::date";
  const auto days = co_await tx.exec(
      "SELECT to_char(period_start, 'YYYY-MM-DD') AS d, status, coalesce(final, base)::float8 AS v, "
      "  breakdown->'evidence_minutes' AS mins, (" + endExpr + " - $2::date + 1) AS n "
      "FROM scores WHERE user_id = $1::uuid AND period = 'day' AND period_start BETWEEN $2::date AND " + endExpr +
      " ORDER BY period_start",
      user, start);
  std::string spanEnd = endExpr;
  for (std::size_t p; (p = spanEnd.find("$2")) != std::string::npos;) spanEnd.replace(p, 2, "$1");
  const auto span = co_await tx.exec("SELECT (" + spanEnd + " - $1::date + 1)::int AS n, "
                                     "extract(epoch FROM $1::date::timestamptz)::bigint AS f, "
                                     "extract(epoch FROM (" + spanEnd + " + 1)::timestamptz)::bigint AS t",
                                     start);
  Json::Value breakdown;
  breakdown["days"] = Json::Value(Json::arrayValue);
  double sum = 0;
  int scored = 0;
  std::map<std::string, double> areaMinutes;
  for (const auto& d : days) {
    Json::Value j;
    j["day"] = d["d"].as<std::string>(), j["status"] = d["status"].as<std::string>();
    const auto st = d["status"].as<std::string>();
    if ((st == "final" || st == "provisional") && !d["v"].isNull()) {
      j["score"] = d["v"].as<double>();
      sum += d["v"].as<double>();
      ++scored;
    }
    if (!d["mins"].isNull()) {
      const auto m = json::parse(d["mins"].as<std::string>());
      for (const auto& a : m.getMemberNames()) areaMinutes[a] += m[a].asDouble();
    }
    breakdown["days"].append(j);
  }
  Json::Value coverage;
  coverage["scored_days"] = scored;
  coverage["days"] = span[0]["n"].as<int>();
  NewVersion v;
  v.user = user, v.period = period, v.start = start, v.coverage = coverage;
  v.consistency = Json::Value(Json::objectValue);
  if (scored == 0) {
    v.status = "not_scored";
    breakdown["reason"] = "no scored days in the period";
  } else {
    double score = sum / scored;
    // Weekly targets: 20 % of the week score (§10). Minutes from the days' resolved evidence per area.
    if (period == "week") {
      std::vector<double> ratios;
      breakdown["weekly_targets"] = Json::Value(Json::arrayValue);
      for (const auto& t : co_await tx.exec(
               "SELECT id::text AS id, title, area, metric, amount::float8 AS amount FROM targets "
               "WHERE deleted_at IS NULL AND period = 'week' AND (active_from IS NULL OR active_from <= $1::date + 6) "
               "  AND (active_to IS NULL OR active_to >= $1::date)",
               start)) {
        const auto area = t["area"].as<std::string>(), metric = t["metric"].as<std::string>();
        double achieved = 0;
        if (metric == "minutes") {
          achieved = areaMinutes[area];
        } else {
          const auto c = co_await tx.exec(
              "SELECT (SELECT count(*) FROM activities WHERE deleted_at IS NULL AND area::text = $3 "
              "          AND started_at >= to_timestamp($1::bigint) AND started_at < to_timestamp($2::bigint))::float8 AS acts, "
              "       (SELECT coalesce(sum(greatest(page_to - page_from, 0)), 0) FROM skill_sessions WHERE deleted_at IS NULL "
              "          AND started_at >= to_timestamp($1::bigint) AND started_at < to_timestamp($2::bigint))::float8 AS pages, "
              "       (SELECT count(*) FROM skill_sessions WHERE deleted_at IS NULL "
              "          AND started_at >= to_timestamp($1::bigint) AND started_at < to_timestamp($2::bigint))::float8 AS sess",
              std::to_string(span[0]["f"].as<int64_t>()), std::to_string(span[0]["t"].as<int64_t>()), area);
          achieved = metric == "pages" ? c[0]["pages"].as<double>()
                     : metric == "sessions" ? c[0]["sess"].as<double>() + c[0]["acts"].as<double>()
                                            : c[0]["acts"].as<double>();
        }
        const double ratio = std::min(achieved / t["amount"].as<double>(), 1.0);
        ratios.push_back(ratio);
        Json::Value j;
        j["id"] = t["id"].as<std::string>(), j["title"] = t["title"].as<std::string>(), j["metric"] = metric;
        j["amount"] = t["amount"].as<double>(), j["achieved"] = achieved, j["ratio"] = std::round(ratio * 100) / 100;
        breakdown["weekly_targets"].append(j);
      }
      if (!ratios.empty()) {
        double mean = 0;
        for (const double x : ratios) mean += x;
        mean /= static_cast<double>(ratios.size());
        breakdown["daily_mean"] = std::round(score * 100) / 100;
        score = 0.8 * score + 0.2 * 100.0 * mean;
      }
    }
    v.status = "final";
    v.base = std::round(score * 100) / 100;
    v.final = v.base;
  }
  v.breakdown = breakdown;
  const auto digest = crypto::sha256(json::compact(breakdown));
  static constexpr char kHex[] = "0123456789abcdef";
  for (const auto c : digest) v.hash += std::string{kHex[c >> 4], kHex[c & 0x0F]};
  const auto cur = co_await tx.exec(
      "SELECT inputs_hash FROM score_versions WHERE user_id = $1::uuid AND period = $2 AND period_start = $3::date "
      "AND is_current",
      user, period, start);
  if (cur.empty() || cur[0]["inputs_hash"].as<std::string>() != v.hash) co_await insertVersion(tx, v);
  co_await tx.commit();
}

}  // namespace

drogon::Task<void> enqueueDayScoring(db::TenantTx& tx, const std::string& userId, const std::string& localDay) {
  // The day itself (after its summaries), and the day before again: completions logged within 24 h of a close
  // count half (§4.1), and anything else late changes the inputs hash.
  const auto prev = co_await tx.exec("SELECT to_char($1::date - 1, 'YYYY-MM-DD') AS d", localDay);
  std::vector<std::pair<std::string, std::string>> runs;
  runs.emplace_back(localDay, "score:");
  runs.emplace_back(prev[0]["d"].as<std::string>(), "rescore:");
  for (const auto& [day, key] : runs) {
    db::jobs::NewJob j;
    j.kind = "scores.close_day";
    j.payload["user_id"] = userId;
    j.payload["local_day"] = day;
    j.dedupeKey = key + userId + ":" + day;
    j.delaySec = 600;
    j.priority = 2;
    co_await db::jobs::enqueue(tx, j);
  }
}

drogon::Task<void> enqueuePeriodScoring(db::TenantTx& tx, const std::string& userId, const std::string& period,
                                        const std::string& periodStart) {
  db::jobs::NewJob j;
  j.kind = "scores.close_period";
  j.payload["user_id"] = userId;
  j.payload["period"] = period;
  j.payload["period_start"] = periodStart;
  j.dedupeKey = "period:" + userId + ":" + period + ":" + periodStart;
  j.delaySec = 1200;  // after the last day's score
  co_await db::jobs::enqueue(tx, j);
}

void registerJobs() {
  jobs::registerHandler("scores.close_day", closeDay);
  jobs::registerHandler("scores.close_period", closePeriod);
}

}  // namespace thelife::scoring
