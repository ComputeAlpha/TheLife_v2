// Day close and episodic memory jobs (agent_lld.md §6.2, §6.4; backend_lld.md §10).
//   day.close              close the day's conversations, mark unfinished routine occurrences missed, queue the
//                          summaries; on Sundays roll up the week and decay registry salience
//   memory.compress_day    one structured LLM call per channel and day: the day summary plus durable facts
//                          (registry extraction is folded into this call, so a day costs one request per channel)
//   memory.rollup          week and month summaries from the day summaries
#include "memory/jobs.h"

#include <drogon/drogon.h>

#include "agent/prompt.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/job_store.h"
#include "jobs/worker.h"
#include "llm/gateway.h"
#include "memory/registry.h"
#include "memory/semantic.h"
#include "scoring/jobs.h"
#include "tools/registry.h"

namespace thelife::memory {

namespace {

using drogon::Task;
using jobs::Job;

auth::Principal systemFor(const std::string& tenant, const std::string& user) {
  auth::Principal p;
  p.kind = auth::PrincipalKind::System;
  p.tenantId = tenant;
  p.userId = user;
  return p;
}

std::string modelName() {
  const auto& ps = llm::gateway().config().providers;
  return ps.empty() ? "none" : ps.front().chatModel;
}

std::string promptVersion(const agent::Prompt& p) { return p.version + "+" + p.hash; }

// Model unavailable or rate limited is not a job failure: run again when it can work.
[[noreturn]] void rethrowForJobs(const llm::LlmError& e) {
  if (e.cls == llm::ErrorClass::ProviderUnavailable) {
    throw jobs::Defer(e.resetsAt.value_or(llm::Clock::now() + std::chrono::hours(1)), "assistant offline");
  }
  if (e.cls == llm::ErrorClass::RateLimited) {
    throw jobs::Defer(llm::Clock::now() + std::chrono::seconds(e.retryAfterSec.value_or(60)), "rate limited");
  }
  throw std::runtime_error(std::string("llm: ") + llm::toString(e.cls));
}

Task<Json::Value> structured(llm::LlmRequest req, std::string name, Json::Value schema) {
  std::optional<llm::LlmError> err;
  Json::Value out;
  try {
    out = co_await llm::gateway().structured(std::move(req), std::move(name), std::move(schema));
  } catch (const llm::LlmError& e) {
    err = e;
  }
  if (err) rethrowForJobs(*err);
  co_return out;
}

const Json::Value& daySchema() {
  static const Json::Value s = json::parse(R"({
    "type": "object", "additionalProperties": false, "required": ["summary", "topics", "facts"],
    "properties": {
      "summary": {"type": "string", "minLength": 1, "maxLength": 4000},
      "topics": {"type": "array", "maxItems": 8, "items": {"type": "string", "minLength": 1, "maxLength": 60}},
      "facts": {"type": "array", "maxItems": 10, "items": {
        "type": "object", "additionalProperties": false, "required": ["subject", "statement", "confidence"],
        "properties": {
          "subject": {"type": "string", "minLength": 1, "maxLength": 120},
          "subject_kind": {"type": "string", "enum": ["self", "person", "project", "goal", "habit", "preference",
                                                      "place", "app", "topic", "other"]},
          "statement": {"type": "string", "minLength": 1, "maxLength": 280},
          "confidence": {"type": "number", "minimum": 0, "maximum": 1},
          "contradicts_known": {"type": "boolean"}}}}}})");
  return s;
}

const Json::Value& rollupSchema() {
  static const Json::Value s = json::parse(R"({"type": "object", "additionalProperties": false,
    "required": ["summary"], "properties": {"summary": {"type": "string", "minLength": 1, "maxLength": 4000}}})");
  return s;
}

Task<void> storeSummary(db::TenantTx& tx, const std::string& user, const std::string& channel,
                        const std::string& period, const std::string& start, const std::string& content,
                        const Json::Value& topics, const std::string& promptVer) {
  std::string arr = "{";
  for (Json::ArrayIndex i = 0; i < topics.size(); ++i) {
    std::string t = topics[i].asString();
    std::string esc;
    for (char ch : t) {
      if (ch == '"' || ch == '\\') esc += '\\';
      esc += ch;
    }
    arr += (i ? "," : "") + std::string("\"") + esc + "\"";
  }
  arr += "}";
  std::vector<std::optional<std::string>> params{ids::newUuidV7(), tx.tenantId(), user, channel, period, start,
                                                 content, arr, modelName(), promptVer};
  co_await tx.execParams(
      "INSERT INTO episodic_summaries (id, tenant_id, user_id, channel, period, period_start, content, topics, model, "
      "prompt_version) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6::date, $7, $8::text[], $9, $10) "
      "ON CONFLICT (tenant_id, user_id, channel, period, period_start) DO UPDATE SET content = EXCLUDED.content, "
      "topics = EXCLUDED.topics, model = EXCLUDED.model, prompt_version = EXCLUDED.prompt_version, created_at = now()",
      std::move(params));
}

// ------------------------------------------------------------------------------------------------ day.close

Task<void> dayClose(const Job& job) {
  const auto user = job.payload["user_id"].asString();
  const auto day = job.payload["local_day"].asString();
  auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
  co_await tx.exec(
      "UPDATE conversations SET status = 'closed', closed_at = now() "
      "WHERE user_id = $1::uuid AND status = 'open' AND local_day <= $2::date",
      user, day);

  // Unfinished routine occurrences of the closed day become missed (backend_lld.md §6.3).
  const auto w = co_await tx.exec(
      "SELECT to_char(($2::date + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "               'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS f, "
      "       to_char((($2::date + 1) + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "               'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS t, extract(isodow FROM $2::date)::int AS dow, "
      "       ($2::date + 1 = date_trunc('month', $2::date + 1)::date) AS month_end "
      "FROM users WHERE id = $1::uuid",
      user, day);
  if (w.empty()) {
    co_await tx.commit();
    co_return;
  }
  Json::Value args;
  args["from"] = w[0]["f"].as<std::string>();
  args["to"] = w[0]["t"].as<std::string>();
  const auto cal = co_await tools::registry().runInTx("calendar.view", systemFor(job.tenantId, user), tx, args);
  for (const auto& e : cal["items"]) {
    if (e["kind"].asString() != "task_occurrence" || !e["all_day"].asBool() || e["status"].asString() != "todo" ||
        e["occurs_on"].asString() != day) {
      continue;
    }
    co_await tx.exec(
        "INSERT INTO task_occurrences (id, tenant_id, created_by_kind, task_id, occurs_on, status) "
        "VALUES ($1::uuid, $2::uuid, 'system', $3::uuid, $4::date, 'missed') "
        "ON CONFLICT (tenant_id, task_id, occurs_on) DO NOTHING",
        ids::newUuidV7(), job.tenantId, e["id"].asString(), day);
  }

  const auto channels = co_await tx.exec(
      "SELECT DISTINCT c.channel FROM conversations c WHERE c.user_id = $1::uuid AND c.local_day = $2::date "
      "  AND EXISTS (SELECT 1 FROM messages m WHERE m.conversation_id = c.id AND m.role = 'user')",
      user, day);
  for (const auto& ch : channels) {
    db::jobs::NewJob j;
    j.kind = "memory.compress_day";
    j.payload["user_id"] = user;
    j.payload["channel"] = ch["channel"].as<std::string>();
    j.payload["local_day"] = day;
    j.dedupeKey = user + ":" + ch["channel"].as<std::string>() + ":" + day;
    co_await db::jobs::enqueue(tx, j);
  }

  const bool sunday = w[0]["dow"].as<int>() == 7;
  const bool monthEnd = w[0]["month_end"].as<bool>();
  co_await scoring::enqueueDayScoring(tx, user, day);
  if (sunday) {
    const auto ws = co_await tx.exec("SELECT to_char($1::date - 6, 'YYYY-MM-DD') AS s", day);
    co_await scoring::enqueuePeriodScoring(tx, user, "week", ws[0]["s"].as<std::string>());
  }
  if (monthEnd) {
    const auto ms = co_await tx.exec("SELECT to_char(date_trunc('month', $1::date), 'YYYY-MM-DD') AS s", day);
    co_await scoring::enqueuePeriodScoring(tx, user, "month", ms[0]["s"].as<std::string>());
  }
  if (sunday || monthEnd) {
    const auto all = co_await tx.exec(
        "SELECT DISTINCT channel FROM conversations WHERE user_id = $1::uuid AND local_day > $2::date - 31",
        user, day);
    for (const auto& ch : all) {
      for (const char* period : {"week", "month"}) {
        if ((std::string(period) == "week" && !sunday) || (std::string(period) == "month" && !monthEnd)) continue;
        const auto start = co_await tx.exec(
            std::string(period) == "week" ? "SELECT to_char($1::date - 6, 'YYYY-MM-DD') AS s"
                                          : "SELECT to_char(date_trunc('month', $1::date), 'YYYY-MM-DD') AS s",
            day);
        db::jobs::NewJob j;
        j.kind = "memory.rollup";
        j.payload["user_id"] = user;
        j.payload["channel"] = ch["channel"].as<std::string>();
        j.payload["period"] = period;
        j.payload["period_start"] = start[0]["s"].as<std::string>();
        j.dedupeKey = user + ":" + ch["channel"].as<std::string>() + ":" + period + ":" + start[0]["s"].as<std::string>();
        j.delaySec = 300;  // after the day summaries of the period
        co_await db::jobs::enqueue(tx, j);
      }
    }
  }
  if (sunday) {
    // Weekly salience decay (registry.decay): what was not used for a week fades, pinned entities stay.
    co_await tx.exec(
        "UPDATE kg_entities SET salience = greatest(0.05, salience * 0.9) "
        "WHERE user_id = $1::uuid AND NOT pinned AND (last_used_at IS NULL OR last_used_at < now() - interval '7 days')",
        user);
  }
  co_await tx.commit();
}

// ------------------------------------------------------------------------------------------------ compress_day

Task<void> compressDay(const Job& job) {
  const auto user = job.payload["user_id"].asString();
  const auto channel = job.payload["channel"].asString();
  const auto day = job.payload["local_day"].asString();
  std::string transcript, conversationId, userName, known;
  {
    auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
    const auto conv = co_await tx.exec(
        "SELECT c.id::text AS id, u.display_name FROM conversations c JOIN users u ON u.id = c.user_id "
        "WHERE c.user_id = $1::uuid AND c.channel = $2 AND c.local_day = $3::date",
        user, channel, day);
    if (conv.empty()) {
      co_await tx.commit();
      co_return;
    }
    conversationId = conv[0]["id"].as<std::string>();
    userName = conv[0]["display_name"].as<std::string>();
    const auto msgs = co_await tx.exec(
        "SELECT role, content, tool_calls::text AS calls FROM messages WHERE conversation_id = $1::uuid "
        "  AND role IN ('user', 'assistant') AND kind <> 'rolling_summary' ORDER BY created_at, id",
        conversationId);
    for (const auto& m : msgs) {
      const auto role = m["role"].as<std::string>();
      const auto content = m["content"].as<std::string>();
      if (!content.empty()) transcript += (role == "user" ? "User: " : "Assistant: ") + content + "\n";
      for (const auto& c : json::parse(m["calls"].as<std::string>())) {
        transcript += "[" + c["name"].asString() + " " + c["arguments"].asString().substr(0, 200) + "]\n";
      }
    }
    const auto facts = co_await tx.exec(
        "SELECT f.statement, f.origin::text AS origin FROM kg_facts f WHERE f.user_id = $1::uuid "
        "  AND f.status = 'active' AND f.deleted_at IS NULL ORDER BY f.created_at DESC LIMIT 40",
        user);
    for (const auto& f : facts) {
      known += "- " + f["statement"].as<std::string>() +
               (f["origin"].as<std::string>() == "inferred" ? "" : " (stated by the user)") + "\n";
    }
    co_await tx.commit();
  }
  if (transcript.empty()) co_return;
  // Long days: keep the start and the end.
  if (transcript.size() > 60000) transcript = transcript.substr(0, 10000) + "\n[...]\n" + transcript.substr(transcript.size() - 50000);

  const auto p = agent::prompt("jobs/compress_day");
  llm::LlmRequest req;
  req.tenantId = job.tenantId;
  req.purpose = "memory.compress_day";
  req.priority = llm::Priority::Background;
  req.reasoningEffort = "low";
  req.promptId = p.id;
  req.promptVersion = promptVersion(p);
  req.maxTokens = 3000;
  req.messages = {llm::Message::system(p.render({{"user_name", userName},
                                                 {"day", day},
                                                 {"channel", channel},
                                                 {"known_facts", known.empty() ? "(none yet)" : known}})),
                  llm::Message::user(transcript)};
  const auto out = co_await structured(std::move(req), "day_summary", daySchema());

  auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
  co_await storeSummary(tx, user, channel, "day", day, out["summary"].asString(), out["topics"], promptVersion(p));
  for (const auto& f : out["facts"]) {
    NewFact nf;
    nf.userId = user;
    nf.subject = f["subject"].asString();
    nf.subjectKind = f.get("subject_kind", "self").asString();
    nf.statement = f["statement"].asString();
    nf.confidence = f["confidence"].asDouble();
    // Confident and not contradicting the user → used right away; otherwise held for review (§6.4).
    nf.status = nf.confidence >= 0.7 && !f.get("contradicts_known", false).asBool() ? "active" : "proposed";
    nf.createdByKind = "agent";
    nf.origin = "inferred";
    Json::Value ref;
    ref["type"] = "conversation";
    ref["id"] = conversationId;
    nf.sourceRefs.append(ref);
    co_await addFact(tx, std::move(nf));
  }
  co_await enqueueSync(tx);
  co_await tx.commit();
}

// ------------------------------------------------------------------------------------------------ rollup

Task<void> rollup(const Job& job) {
  const auto user = job.payload["user_id"].asString();
  const auto channel = job.payload["channel"].asString();
  const auto period = job.payload["period"].asString();
  const auto start = job.payload["period_start"].asString();
  std::string sources, userName;
  {
    auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
    const auto end = period == "week" ? "$3::date + 6" : "($3::date + interval '1 month' - interval '1 day')::date";
    const auto rows = co_await tx.exec(
        "SELECT to_char(s.period_start, 'YYYY-MM-DD') AS d, s.content, u.display_name FROM episodic_summaries s "
        "JOIN users u ON u.id = s.user_id WHERE s.user_id = $1::uuid AND s.channel = $2 AND s.period = 'day' "
        "  AND s.period_start BETWEEN $3::date AND " + std::string(end) + " ORDER BY s.period_start",
        user, channel, start);
    co_await tx.commit();
    for (const auto& r : rows) {
      sources += r["d"].as<std::string>() + ":\n" + r["content"].as<std::string>() + "\n\n";
      userName = r["display_name"].as<std::string>();
    }
  }
  if (sources.empty()) co_return;
  const auto p = agent::prompt("jobs/rollup");
  llm::LlmRequest req;
  req.tenantId = job.tenantId;
  req.purpose = "memory.rollup_" + period;
  req.priority = llm::Priority::Background;
  req.reasoningEffort = "low";
  req.promptId = p.id;
  req.promptVersion = promptVersion(p);
  req.maxTokens = 2000;
  req.messages = {llm::Message::system(p.render(
                      {{"user_name", userName}, {"period", period}, {"period_start", start}})),
                  llm::Message::user(sources.substr(0, 60000))};
  const auto out = co_await structured(std::move(req), "summary", rollupSchema());
  auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
  co_await storeSummary(tx, user, channel, period, start, out["summary"].asString(), Json::Value(Json::arrayValue),
                        promptVersion(p));
  co_await tx.commit();
}

}  // namespace

void registerJobs() {
  jobs::registerHandler("day.close", dayClose);
  jobs::registerHandler("memory.compress_day", compressDay);
  jobs::registerHandler("memory.rollup", rollup);
}

}  // namespace thelife::memory
