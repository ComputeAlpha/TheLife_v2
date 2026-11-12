// Calendar events (api/resources/calendar.yaml) and calendar.view: one merged, time-ordered list of events
// (recurring ones expanded), task occurrences, entertainment slots and skill sessions for a window.
#include <algorithm>
#include <map>

#include "core/errors.h"
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

constexpr int kMaxWindowDays = 62;
constexpr size_t kMaxEntries = 2000;

struct Window {
  std::string from, to;              // instants, ISO 8601 UTC
  LocalDateTime fromLocal, toLocal;  // wall clock in the user's zone
  LocalDateTime fromDay, toDay;      // local days honouring day_start_hour (00:00 of each)
  std::string timezone;
  int dayStartHour = 4;
};

Json::Value entry(const char* kind, const std::string& id, const std::string& title, const std::string& startsAt,
                  const std::string& endsAt, bool allDay, const Json::Value& area, const Json::Value& status,
                  const Json::Value& occursOn) {
  Json::Value e;
  e["kind"] = kind;
  e["id"] = id;
  e["title"] = title;
  e["starts_at"] = startsAt;
  e["ends_at"] = endsAt;
  e["all_day"] = allDay;
  e["area"] = area;
  e["status"] = status;
  e["occurs_on"] = occursOn;
  return e;
}

Json::Value textOrNull(const drogon::orm::Field& f) {
  return f.isNull() ? Json::Value() : Json::Value(f.as<std::string>());
}

Task<Window> resolveWindow(ToolCall& c) {
  auto opt = [&](const char* k) {
    return c.args.isMember(k) ? std::optional<std::string>(c.args[k].asString()) : std::optional<std::string>();
  };
  // (built before co_await: a braced list inside the co_await expression crashes GCC 13/14)
  const std::vector<std::optional<std::string>> params{c.principal.userId, opt("from"), opt("to"),
                                                       std::to_string(kMaxWindowDays)};
  const auto rows = co_await c.tx.execParams(
      "WITH w AS (SELECT coalesce($2::timestamptz, now()) AS f, "
      "                  coalesce($3::timestamptz, coalesce($2::timestamptz, now()) + interval '7 days') AS t) "
      "SELECT u.timezone, u.day_start_hour::int AS dsh, "
      "  to_jsonb(w.f) #>> '{}' AS f, to_jsonb(w.t) #>> '{}' AS t, "
      "  w.t > w.f AS ordered, w.t - w.f <= make_interval(days => $4::int) AS bounded, "
      "  to_char(w.f AT TIME ZONE u.timezone, 'YYYY-MM-DD HH24:MI:SS') AS f_local, "
      "  to_char(w.t AT TIME ZONE u.timezone, 'YYYY-MM-DD HH24:MI:SS') AS t_local, "
      "  to_char(((w.f AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date, 'YYYY-MM-DD') "
      "    AS f_day, "
      "  to_char(((w.t AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date, 'YYYY-MM-DD') "
      "    AS t_day "
      "FROM users u, w WHERE u.id = $1::uuid",
      params);
  if (rows.empty()) throw ApiError::unauthorized();
  const auto& r = rows[0];
  if (!r["ordered"].as<bool>()) throw ApiError::validation({{"to", "must be after from"}});
  if (!r["bounded"].as<bool>()) {
    throw ApiError::validation({{"to", "window must not exceed " + std::to_string(kMaxWindowDays) + " days"}});
  }
  Window w;
  w.from = r["f"].as<std::string>();
  w.to = r["t"].as<std::string>();
  w.fromLocal = *LocalDateTime::parse(r["f_local"].as<std::string>());
  w.toLocal = *LocalDateTime::parse(r["t_local"].as<std::string>());
  w.fromDay = *LocalDateTime::parse(r["f_day"].as<std::string>());
  w.toDay = *LocalDateTime::parse(r["t_day"].as<std::string>());
  w.timezone = r["timezone"].as<std::string>();
  w.dayStartHour = r["dsh"].as<int>();
  co_return w;
}

// Instants are UTC ISO strings from Postgres; same format, so string order is time order.
bool overlaps(const std::string& start, const std::string& end, const Window& w) {
  return start < w.to && (end > w.from || (end == start && start >= w.from));
}

Task<void> addEvents(ToolCall& c, const Window& w, std::vector<Json::Value>& out) {
  const auto single = co_await c.tx.exec(
      "SELECT e.id::text, e.title, to_jsonb(e.starts_at) #>> '{}' AS s, to_jsonb(e.ends_at) #>> '{}' AS en, "
      "  e.all_day, e.area::text AS area FROM calendar_events e "
      "WHERE e.deleted_at IS NULL AND e.rrule IS NULL AND e.starts_at < $2::timestamptz "
      "  AND e.ends_at >= $1::timestamptz",
      w.from, w.to);
  for (const auto& r : single) {
    const auto s = r["s"].as<std::string>(), en = r["en"].as<std::string>();
    if (!overlaps(s, en, w)) continue;
    out.push_back(entry("event", r["id"].as<std::string>(), r["title"].as<std::string>(), s, en,
                        r["all_day"].as<bool>(), textOrNull(r["area"]), Json::Value(), Json::Value()));
  }

  const auto recurring = co_await c.tx.exec(
      "SELECT e.id::text, e.title, e.all_day, e.area::text AS area, e.rrule, "
      "  to_char(e.starts_at AT TIME ZONE $2, 'YYYY-MM-DD HH24:MI:SS') AS dtstart, "
      "  extract(epoch FROM e.ends_at - e.starts_at)::bigint AS dur "
      "FROM calendar_events e WHERE e.deleted_at IS NULL AND e.rrule IS NOT NULL AND e.starts_at < $1::timestamptz",
      w.to, w.timezone);
  struct Occ {
    size_t row;
    LocalDateTime start;
  };
  std::vector<Occ> occs;
  std::vector<std::pair<LocalDateTime, LocalDateTime>> local;
  for (size_t i = 0; i < recurring.size(); ++i) {
    const auto& r = recurring[i];
    const auto dtstart = LocalDateTime::parse(r["dtstart"].as<std::string>());
    const auto dur = r["dur"].as<long long>();
    if (!dtstart) continue;
    for (const auto& s : recurrence::expand(r["rrule"].as<std::string>(), *dtstart, w.fromLocal.addSeconds(-dur),
                                            w.toLocal, kMaxEntries)) {
      occs.push_back({i, s});
      local.emplace_back(s, s.addSeconds(dur));
    }
  }
  const auto instants = co_await toInstants(c.tx, w.timezone, local);
  for (size_t k = 0; k < occs.size() && k < instants.size(); ++k) {
    const auto& [s, en] = instants[k];
    if (!overlaps(s, en, w)) continue;
    const auto& r = recurring[occs[k].row];
    out.push_back(entry("event", r["id"].as<std::string>(), r["title"].as<std::string>(), s, en,
                        r["all_day"].as<bool>(), textOrNull(r["area"]), Json::Value(), occs[k].start.date()));
  }
}

Task<void> addTaskOccurrences(ToolCall& c, const Window& w, std::vector<Json::Value>& out) {
  // one-off tasks: due within the window
  const auto due = co_await c.tx.exec(
      "SELECT t.id::text, t.title, t.area::text AS area, t.status, to_jsonb(t.due_at) #>> '{}' AS due, "
      "  to_char(((t.due_at AT TIME ZONE $3) - make_interval(hours => $4::int))::date, 'YYYY-MM-DD') AS day "
      "FROM tasks t WHERE t.deleted_at IS NULL AND t.rrule IS NULL AND t.status <> 'cancelled' "
      "  AND t.due_at >= $1::timestamptz AND t.due_at < $2::timestamptz",
      w.from, w.to, w.timezone, w.dayStartHour);
  for (const auto& r : due) {
    const auto at = r["due"].as<std::string>();
    out.push_back(entry("task_occurrence", r["id"].as<std::string>(), r["title"].as<std::string>(), at, at, false,
                        textOrNull(r["area"]), r["status"].as<std::string>() == "done" ? "done" : "todo",
                        r["day"].as<std::string>()));
  }

  // recurring open tasks: one all-day entry per local day
  const auto recurring = co_await c.tx.exec(
      "SELECT t.id::text, t.title, t.area::text AS area, t.rrule, "
      "  to_char((t.rrule_dtstart AT TIME ZONE $2) - make_interval(hours => $3::int), 'YYYY-MM-DD HH24:MI:SS') "
      "    AS dtstart "
      "FROM tasks t WHERE t.deleted_at IS NULL AND t.rrule IS NOT NULL AND t.status = 'open' "
      "  AND t.rrule_dtstart < $1::timestamptz",
      w.to, w.timezone, w.dayStartHour);
  if (recurring.empty()) co_return;
  std::map<std::pair<std::string, std::string>, std::string> state;  // (task, day) → stored status
  const auto stored = co_await c.tx.exec(
      "SELECT o.task_id::text AS t, to_char(o.occurs_on, 'YYYY-MM-DD') AS d, o.status FROM task_occurrences o "
      "WHERE o.deleted_at IS NULL AND o.occurs_on BETWEEN $1::date AND $2::date",
      w.fromDay.date(), w.toDay.date());
  for (const auto& r : stored) state[{r["t"].as<std::string>(), r["d"].as<std::string>()}] = r["status"].as<std::string>();

  struct Occ {
    size_t row;
    std::string day;
  };
  std::vector<Occ> occs;
  std::vector<std::pair<LocalDateTime, LocalDateTime>> local;
  for (size_t i = 0; i < recurring.size(); ++i) {
    const auto& r = recurring[i];
    const auto dtstart = LocalDateTime::parse(r["dtstart"].as<std::string>());
    if (!dtstart) continue;
    for (const auto& s : recurrence::expand(r["rrule"].as<std::string>(), *dtstart, w.fromDay,
                                            w.toDay.addDays(1), kMaxEntries)) {
      const auto dayStart = s.startOfDay().addSeconds(3600LL * w.dayStartHour);
      occs.push_back({i, s.date()});
      local.emplace_back(dayStart, dayStart.addDays(1));
    }
  }
  const auto instants = co_await toInstants(c.tx, w.timezone, local);
  for (size_t k = 0; k < occs.size() && k < instants.size(); ++k) {
    const auto& [s, en] = instants[k];
    if (!overlaps(s, en, w)) continue;
    const auto& r = recurring[occs[k].row];
    const auto id = r["id"].as<std::string>();
    auto it = state.find({id, occs[k].day});
    out.push_back(entry("task_occurrence", id, r["title"].as<std::string>(), s, en, true, textOrNull(r["area"]),
                        it == state.end() ? "todo" : it->second, occs[k].day));
  }
}

Task<void> addSlotsAndSessions(ToolCall& c, const Window& w, std::vector<Json::Value>& out) {
  const auto slots = co_await c.tx.exec(
      "SELECT s.id::text, coalesce(i.title, 'Entertainment') AS title, to_jsonb(s.starts_at) #>> '{}' AS s, "
      "  to_jsonb(s.ends_at) #>> '{}' AS en, s.consumed FROM entertainment_slots s "
      "LEFT JOIN entertainment_items i ON i.id = s.item_id AND i.deleted_at IS NULL "
      "WHERE s.deleted_at IS NULL AND s.starts_at < $2::timestamptz AND s.ends_at > $1::timestamptz",
      w.from, w.to);
  for (const auto& r : slots) {
    out.push_back(entry("entertainment_slot", r["id"].as<std::string>(), r["title"].as<std::string>(),
                        r["s"].as<std::string>(), r["en"].as<std::string>(), false, "entertainment",
                        r["consumed"].as<bool>() ? "consumed" : "planned", Json::Value()));
  }
  const auto sessions = co_await c.tx.exec(
      "SELECT s.id::text, r.title, to_jsonb(s.started_at) #>> '{}' AS s, to_jsonb(s.ended_at) #>> '{}' AS en "
      "FROM skill_sessions s JOIN skill_resources r ON r.id = s.resource_id "
      "WHERE s.deleted_at IS NULL AND s.started_at < $2::timestamptz AND s.ended_at >= $1::timestamptz",
      w.from, w.to);
  for (const auto& r : sessions) {
    const auto s = r["s"].as<std::string>(), en = r["en"].as<std::string>();
    if (!overlaps(s, en, w)) continue;
    out.push_back(entry("skill_session", r["id"].as<std::string>(), r["title"].as<std::string>(), s, en, false,
                        "skill", Json::Value(), Json::Value()));
  }
}

Task<Json::Value> view(ToolCall& c) {
  const auto w = co_await resolveWindow(c);
  std::vector<Json::Value> entries;
  co_await addEvents(c, w, entries);
  co_await addTaskOccurrences(c, w, entries);
  co_await addSlotsAndSessions(c, w, entries);
  std::stable_sort(entries.begin(), entries.end(), [](const Json::Value& a, const Json::Value& b) {
    if (a["starts_at"].asString() != b["starts_at"].asString()) {
      return a["starts_at"].asString() < b["starts_at"].asString();
    }
    return a["kind"].asString() < b["kind"].asString();
  });
  const bool truncated = entries.size() > kMaxEntries;
  if (truncated) entries.resize(kMaxEntries);
  Json::Value out;
  out["truncated"] = truncated;
  out["items"] = Json::Value(Json::arrayValue);
  for (auto& e : entries) out["items"].append(std::move(e));
  co_return out;
}

}  // namespace

void bindCalendar(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "events",
                       .table = "calendar_events",
                       // a recurring event may occur in the window although its first occurrence is earlier
                       .filters = {{"from", "(r.ends_at >= $?::timestamptz OR r.rrule IS NOT NULL)"},
                                   {"to", "r.starts_at < $?::timestamptz"}},
                       .validate = [](Json::Value& f, bool) { requireValidRrule(f, "rrule"); }});
  reg.bind("calendar.view", view);
}

}  // namespace thelife::modules
