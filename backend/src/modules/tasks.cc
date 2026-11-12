// Tasks (api/resources/tasks.yaml): contract CRUD plus occurrences of recurring tasks. Occurrences are expanded
// from the rrule in the user's local day (honouring day_start_hour) and stored only once acted on
// (backend_lld.md §6.3).
#include <map>

#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/recurrence.h"
#include "crud/resource.h"
#include "modules/common.h"
#include "modules/modules.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using recurrence::LocalDateTime;
using tools::ToolCall;

constexpr int kMaxOccurrenceDays = 366;

// A to_jsonb(...)::text column; SQL NULL becomes JSON null.
Json::Value jsonField(const drogon::orm::Field& f) {
  return f.isNull() ? Json::Value() : json::parse(f.as<std::string>());
}

struct LocalTask {
  std::string id;
  std::string status;
  std::optional<std::string> rrule;
  std::optional<LocalDateTime> dtstart;  // local wall time shifted by day_start_hour, so .date() is the day
  std::optional<std::string> dueDay;
  Json::Value completedAt;
};

Task<LocalTask> loadTask(db::TenantTx& tx, const std::string& id, const std::string& userId) {
  const auto rows = co_await tx.exec(
      "SELECT t.status, t.rrule, to_jsonb(t.completed_at)::text AS completed_at, "
      "  to_char((t.rrule_dtstart AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour), "
      "          'YYYY-MM-DD HH24:MI:SS') AS dtstart_local, "
      "  to_char(((t.due_at AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date, "
      "          'YYYY-MM-DD') AS due_day "
      "FROM tasks t JOIN users u ON u.id = $2::uuid WHERE t.id = $1::uuid AND t.deleted_at IS NULL",
      id, userId);
  if (rows.empty()) throw ApiError::notFound();
  const auto& r = rows[0];
  LocalTask t;
  t.id = id;
  t.status = r["status"].as<std::string>();
  if (!r["rrule"].isNull()) {
    t.rrule = r["rrule"].as<std::string>();
    t.dtstart = LocalDateTime::parse(r["dtstart_local"].as<std::string>());
  }
  if (!r["due_day"].isNull()) t.dueDay = r["due_day"].as<std::string>();
  t.completedAt = jsonField(r["completed_at"]);
  co_return t;
}

LocalDateTime parseDay(const Json::Value& v, const char* name) {
  auto d = LocalDateTime::parse(v.asString());
  if (!d || v.asString().size() != 10) throw ApiError::validation({{name, "must be a date (YYYY-MM-DD)"}});
  return *d;
}

Json::Value occurrence(const std::string& taskId, const std::string& day, const std::string& status,
                       const Json::Value& completedAt, const Json::Value& skipReason, bool materialized) {
  Json::Value o;
  o["task_id"] = taskId;
  o["occurs_on"] = day;
  o["status"] = status;
  o["completed_at"] = completedAt;
  o["skip_reason"] = skipReason;
  o["materialized"] = materialized;
  return o;
}

Json::Value fromRow(const std::string& taskId, const drogon::orm::Row& r) {
  return occurrence(taskId, r["d"].as<std::string>(), r["status"].as<std::string>(),
                    jsonField(r["ca"]),
                    r["skip_reason"].isNull() ? Json::Value() : Json::Value(r["skip_reason"].as<std::string>()), true);
}

Task<Json::Value> listOccurrences(ToolCall& c) {
  const auto clock = co_await userClock(c.tx, c.principal.userId);
  const auto task = co_await loadTask(c.tx, c.args["id"].asString(), c.principal.userId);
  const auto from = parseDay(c.args.get("from", clock.today), "from");
  const auto to = c.args.isMember("to") ? parseDay(c.args["to"], "to") : from.addDays(6);
  if (to < from) throw ApiError::validation({{"to", "must not be before from"}});
  if (from.addDays(kMaxOccurrenceDays) < to) {
    throw ApiError::validation({{"to", "range must not exceed " + std::to_string(kMaxOccurrenceDays) + " days"}});
  }

  std::map<std::string, Json::Value> byDay;
  if (task.rrule && task.dtstart) {
    for (const auto& t : recurrence::expand(*task.rrule, *task.dtstart, from, to.addDays(1), kMaxOccurrenceDays + 1)) {
      byDay[t.date()] = occurrence(task.id, t.date(), "todo", Json::Value(), Json::Value(), false);
    }
    const auto rows = co_await c.tx.exec(
        "SELECT to_char(o.occurs_on, 'YYYY-MM-DD') AS d, o.status, to_jsonb(o.completed_at)::text AS ca, "
        "o.skip_reason FROM task_occurrences o "
        "WHERE o.task_id = $1::uuid AND o.occurs_on BETWEEN $2::date AND $3::date AND o.deleted_at IS NULL",
        task.id, from.date(), to.date());
    for (const auto& r : rows) byDay[r["d"].as<std::string>()] = fromRow(task.id, r);
  } else if (task.dueDay && *task.dueDay >= from.date() && *task.dueDay <= to.date()) {
    const std::string status = task.status == "done" ? "done" : task.status == "cancelled" ? "skipped" : "todo";
    byDay[*task.dueDay] = occurrence(task.id, *task.dueDay, status, task.completedAt, Json::Value(), false);
  }

  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  for (auto& [day, item] : byDay) out["items"].append(std::move(item));
  co_return out;
}

Task<Json::Value> actOnOccurrence(ToolCall& c, bool complete) {
  const auto task = co_await loadTask(c.tx, c.args["id"].asString(), c.principal.userId);
  if (!task.rrule || !task.dtstart) {
    throw ApiError::conflict("This task is not recurring; update its status instead.");
  }
  if (task.status != "open") throw ApiError::conflict("The task is " + task.status + ".");
  const auto day = parseDay(c.args["date"], "date");
  if (recurrence::expand(*task.rrule, *task.dtstart, day, day.addDays(1), 1).empty()) {
    throw ApiError::validation({{"date", "is not an occurrence of this task"}});
  }
  const auto actor = c.principal.actorId();
  auto arg = [&](const char* name, bool when) {
    return when && c.args.isMember(name) ? std::optional<std::string>(c.args[name].asString()) : std::nullopt;
  };
  const std::vector<std::optional<std::string>> params{
      ids::newUuidV7(),
      c.principal.tenantId,
      std::string(auth::createdByKind(c.principal.kind)),
      actor.empty() ? std::nullopt : std::optional<std::string>(actor),
      task.id,
      day.date(),
      std::string(complete ? "done" : "skipped"),
      arg("completed_at", complete),
      arg("reason", !complete)};
  const auto rows = co_await c.tx.execParams(
      "INSERT INTO task_occurrences AS o (id, tenant_id, created_by_kind, created_by_id, task_id, occurs_on, status, "
      "  completed_at, skip_reason) "
      "VALUES ($1::uuid, $2::uuid, $3::principal_kind, $4::uuid, $5::uuid, $6::date, $7::text, "
      "  CASE WHEN $7::text = 'done' THEN coalesce($8::timestamptz, now()) END, $9::text) "
      "ON CONFLICT (tenant_id, task_id, occurs_on) DO UPDATE SET status = EXCLUDED.status, "
      "  completed_at = EXCLUDED.completed_at, skip_reason = EXCLUDED.skip_reason, deleted_at = NULL "
      "RETURNING to_char(o.occurs_on, 'YYYY-MM-DD') AS d, o.status, to_jsonb(o.completed_at)::text AS ca, "
      "  o.skip_reason",
      params);
  co_return fromRow(task.id, rows[0]);
}

}  // namespace

void bindTasks(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "tasks",
                       .table = "tasks",
                       .filters = {{"recurring", "(r.rrule IS NOT NULL) = $?::boolean"},
                                   {"due_before", "r.due_at < $?::timestamptz"}},
                       .validate = [](Json::Value& f, bool) { requireValidRrule(f, "rrule"); }});
  const auto resolver = crud::targetResolver("tasks");
  reg.bind("tasks.occurrences", listOccurrences, resolver);
  reg.bind("tasks.complete_occurrence", [](ToolCall& c) { return actOnOccurrence(c, true); }, resolver);
  reg.bind("tasks.skip_occurrence", [](ToolCall& c) { return actOnOccurrence(c, false); }, resolver);
}

}  // namespace thelife::modules
