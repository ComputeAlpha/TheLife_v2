#include "scoring/inputs.h"

#include <map>
#include <set>

#include "core/json.h"
#include "llm/classifier.h"
#include "tools/registry.h"

namespace thelife::scoring {

namespace {

using drogon::Task;

auth::Principal systemFor(const std::string& tenant, const std::string& user) {
  auth::Principal p;
  p.kind = auth::PrincipalKind::System;
  p.tenantId = tenant;
  p.userId = user;
  return p;
}

int64_t epoch(const std::string& rfc3339) {
  const auto t = llm::parseRfc3339(rfc3339);
  return t ? static_cast<int64_t>(llm::Clock::to_time_t(*t)) : 0;
}

double priorityWeight(int p) {
  switch (p) {
    case 0: return 1.0;
    case 2: return 2.0;
    case 3: return 3.0;
    default: return 1.5;  // normal
  }
}

// Completion of a task or deliverable finished at `doneAt` for a day ending at `dayTo` (§4.1): on time 1, within
// 24 h after the close 0.5 (late entry or late completion), later 0.
double lateness(int64_t doneAt, int64_t dayTo) {
  if (doneAt <= 0) return 0;
  if (doneAt <= dayTo) return 1;
  if (doneAt <= dayTo + 86400) return 0.5;
  return 0;
}

std::string arrayLiteral(const std::set<std::string>& ids) {
  std::string s = "{";
  for (const auto& id : ids) s += (s.size() > 1 ? "," : "") + id;
  return s + "}";
}

std::vector<std::string> textArray(const std::string& pg) {  // {a,b} → [a, b]
  std::vector<std::string> out;
  std::string cur;
  for (const char c : pg) {
    if (c == '{' || c == '}' || c == '"') continue;
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool appliesTo(const std::vector<std::string>& profiles, const Profile& p) {
  if (profiles.empty()) return p.scored;  // default: every scored profile
  return std::find(profiles.begin(), profiles.end(), p.name) != profiles.end();
}

}  // namespace

Task<Profile> profileFor(db::TenantTx& tx, const std::string& userId, const std::string& localDay) {
  // Explicit override, then an all-day calendar event day_profile.<name>, then the weekday mapping (§3).
  const auto r = co_await tx.exec(
      "SELECT coalesce("
      "  (SELECT profile FROM day_profile_overrides WHERE local_day = $2::date AND deleted_at IS NULL LIMIT 1), "
      "  (SELECT substring(e.category FROM 13) FROM calendar_events e "
      "    WHERE e.deleted_at IS NULL AND e.all_day AND e.category LIKE 'day\\_profile.%' AND e.rrule IS NULL "
      "      AND e.starts_at < (($2::date + 1) + make_interval(hours => u.day_start_hour)) AT TIME ZONE u.timezone "
      "      AND e.ends_at > ($2::date + make_interval(hours => u.day_start_hour)) AT TIME ZONE u.timezone "
      "    ORDER BY e.created_at DESC LIMIT 1), "
      "  CASE WHEN upper(left(to_char($2::date, 'DY'), 2)) = ANY (u.workdays) THEN 'workday' ELSE 'weekend' END) AS p "
      "FROM users u WHERE u.id = $1::uuid",
      userId, localDay);
  Profile p;
  p.name = r.empty() ? "workday" : r[0]["p"].as<std::string>();
  const auto custom = co_await tx.exec("SELECT scored FROM day_profiles WHERE name = $1 AND deleted_at IS NULL", p.name);
  p.scored = custom.empty() ? (p.name != "leave" && p.name != "sick") : custom[0]["scored"].as<bool>();
  co_return p;
}

Task<Settings> loadSettings(db::TenantTx& tx, const std::string& userId) {
  Settings s;
  const auto r = co_await tx.exec("SELECT weights::text AS w, plan_share::text AS a FROM scoring_settings WHERE user_id = $1::uuid",
                                  userId);
  if (r.empty()) co_return s;
  const auto w = json::parse(r[0]["w"].as<std::string>());
  for (const auto& k : w.getMemberNames()) s.weights[k] = w[k].asDouble();
  const auto a = json::parse(r[0]["a"].as<std::string>());
  for (const auto& k : a.getMemberNames()) s.alpha[k] = a[k].asDouble();
  co_return s;
}

Task<DayInputs> gatherDay(db::TenantTx& tx, const std::string& userId, const std::string& localDay) {
  DayInputs in;
  in.localDay = localDay;
  const auto w = co_await tx.exec(
      "SELECT extract(epoch FROM ($2::date + make_interval(hours => day_start_hour)) AT TIME ZONE timezone)::bigint AS f, "
      "  extract(epoch FROM (($2::date + 1) + make_interval(hours => day_start_hour)) AT TIME ZONE timezone)::bigint AS t, "
      "  to_char(($2::date + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "          'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS fs, "
      "  to_char((($2::date + 1) + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "          'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS ts, "
      "  to_char((($2::date - 6) + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "          'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS ws "
      "FROM users WHERE id = $1::uuid",
      userId, localDay);
  if (w.empty()) throw std::runtime_error("user not found");
  in.dayFrom = w[0]["f"].as<int64_t>();
  in.dayTo = w[0]["t"].as<int64_t>();
  const auto from = std::to_string(in.dayFrom), to = std::to_string(in.dayTo);
  const auto profile = co_await profileFor(tx, userId, localDay);
  in.profile = profile.name;
  in.scored = profile.scored;
  in.settings = co_await loadSettings(tx, userId);
  const auto sys = systemFor(tx.tenantId(), userId);

  // ---- planned items (§4.1) from the merged calendar of the day
  Json::Value window;
  window["from"] = w[0]["fs"].as<std::string>();
  window["to"] = w[0]["ts"].as<std::string>();
  const auto cal = co_await tools::registry().runInTx("calendar.view", sys, tx, window);
  std::set<std::string> taskIds, eventIds;
  for (const auto& e : cal["items"]) {
    if (e["kind"].asString() == "task_occurrence") taskIds.insert(e["id"].asString());
    if (e["kind"].asString() == "event" && !e["all_day"].asBool()) eventIds.insert(e["id"].asString());
  }
  struct TaskInfo {
    int priority = 1;
    std::vector<std::string> profiles;
    bool recurring = false;
    int64_t completedAt = 0;
  };
  std::map<std::string, TaskInfo> tasks;
  struct Occ {
    std::string status;
    int64_t completedAt = 0, changedAt = 0;
  };
  std::map<std::string, Occ> occs;
  if (!taskIds.empty()) {
    for (const auto& r : co_await tx.exec(
             "SELECT id::text AS id, priority, day_profiles::text AS dp, rrule IS NOT NULL AS rec, "
             "  coalesce(extract(epoch FROM completed_at)::bigint, 0) AS ca FROM tasks WHERE id = ANY($1::uuid[])",
             arrayLiteral(taskIds))) {
      tasks[r["id"].as<std::string>()] = {r["priority"].as<int>(), textArray(r["dp"].as<std::string>()),
                                          r["rec"].as<bool>(), r["ca"].as<int64_t>()};
    }
    for (const auto& r : co_await tx.exec(
             "SELECT task_id::text AS id, status, coalesce(extract(epoch FROM completed_at)::bigint, 0) AS ca, "
             "  extract(epoch FROM updated_at)::bigint AS ua FROM task_occurrences "
             "WHERE occurs_on = $1::date AND task_id = ANY($2::uuid[]) AND deleted_at IS NULL",
             localDay, arrayLiteral(taskIds))) {
      occs[r["id"].as<std::string>()] = {r["status"].as<std::string>(), r["ca"].as<int64_t>(), r["ua"].as<int64_t>()};
    }
  }
  struct EventInfo {
    std::string linkType, linkId;
  };
  std::map<std::string, EventInfo> events;
  if (!eventIds.empty()) {
    for (const auto& r : co_await tx.exec(
             "SELECT id::text AS id, coalesce(link_type, '') AS lt, coalesce(link_id::text, '') AS li "
             "FROM calendar_events WHERE id = ANY($1::uuid[])",
             arrayLiteral(eventIds))) {
      events[r["id"].as<std::string>()] = {r["lt"].as<std::string>(), r["li"].as<std::string>()};
    }
  }
  bool progressLogged = false;
  for (const auto& e : cal["items"]) {
    const auto kind = e["kind"].asString();
    const auto area = e["area"].isString() ? e["area"].asString() : "";
    if (kind == "task_occurrence") {
      const auto id = e["id"].asString();
      const auto& t = tasks[id];
      PlanItem it;
      it.type = "task", it.id = id, it.title = e["title"].asString(), it.area = area, it.due = true;
      it.weight = priorityWeight(t.priority);
      if (t.recurring) {
        if (!appliesTo(t.profiles, profile)) continue;
        const auto o = occs.find(id);
        if (o != occs.end()) {
          if (o->second.status == "done") {
            it.completion = lateness(o->second.completedAt, in.dayTo);
            progressLogged = true;
          } else if (o->second.status == "skipped") {
            it.excluded = o->second.changedAt < in.dayFrom;  // skipped before the day began: replanned
            progressLogged = true;
          }
        }
      } else if (e["status"].asString() == "done") {
        it.completion = lateness(t.completedAt, in.dayTo);
        progressLogged = true;
      }
      in.items.push_back(it);
    } else if (kind == "event" && !e["all_day"].asBool() && !area.empty()) {
      PlanItem it;
      it.type = "block", it.id = e["id"].asString(), it.title = e["title"].asString(), it.area = area;
      it.from = epoch(e["starts_at"].asString());
      it.to = epoch(e["ends_at"].asString());
      it.weight = std::max(0.5, static_cast<double>(it.to - it.from) / 3600.0);
      const auto ev = events.find(it.id);
      if (ev != events.end()) it.linkType = ev->second.linkType, it.linkId = ev->second.linkId;
      in.items.push_back(it);
    }
  }
  // Deliverable milestones due that day.
  for (const auto& r : co_await tx.exec(
           "SELECT id::text AS id, title, coalesce(extract(epoch FROM delivered_at)::bigint, 0) AS da "
           "FROM work_deliverables WHERE deleted_at IS NULL AND status <> 'cancelled' "
           "  AND due_at >= to_timestamp($1::bigint) AND due_at < to_timestamp($2::bigint)",
           from, to)) {
    PlanItem it;
    it.type = "deliverable", it.id = r["id"].as<std::string>(), it.title = r["title"].as<std::string>();
    it.area = "work", it.weight = 2, it.due = true;
    it.completion = lateness(r["da"].as<int64_t>(), in.dayTo);
    in.items.push_back(it);
  }

  // ---- evidence (§4.7)
  const auto contexts = co_await tx.exec(
      "SELECT id::text AS id, name, evidence_mode, workday_target_min FROM work_contexts WHERE deleted_at IS NULL "
      "  AND (active_from IS NULL OR active_from <= $1::date) AND (active_to IS NULL OR active_to >= $1::date)",
      localDay);
  bool anyObserved = false;
  for (const auto& c : contexts) anyObserved |= c["evidence_mode"].as<std::string>() == "observed";
  // With only self-reported work contexts, work detections from this machine do not count (§5).
  const bool dropWorkDetections = !contexts.empty() && !anyObserved;
  for (const auto& r : co_await tx.exec(
           "SELECT area::text AS area, coalesce(category, '') AS cat, coalesce(link_type, '') AS lt, "
           "  coalesce(link_id::text, '') AS li, extract(epoch FROM started_at)::bigint AS f, "
           "  extract(epoch FROM ended_at)::bigint AS t FROM activities WHERE deleted_at IS NULL "
           "  AND started_at < to_timestamp($2::bigint) AND ended_at > to_timestamp($1::bigint)",
           from, to)) {
    in.evidence.push_back({r["f"].as<int64_t>(), r["t"].as<int64_t>(), r["area"].as<std::string>(),
                           r["cat"].as<std::string>(), r["lt"].as<std::string>(), r["li"].as<std::string>(), 0});
  }
  for (const auto& r : co_await tx.exec(
           "SELECT resource_id::text AS rid, extract(epoch FROM started_at)::bigint AS f, "
           "  extract(epoch FROM ended_at)::bigint AS t FROM skill_sessions WHERE deleted_at IS NULL "
           "  AND started_at < to_timestamp($2::bigint) AND ended_at > to_timestamp($1::bigint)",
           from, to)) {
    in.evidence.push_back({r["f"].as<int64_t>(), r["t"].as<int64_t>(), "skill", "", "skill_resource",
                           r["rid"].as<std::string>(), 0});
  }
  for (const auto& r : co_await tx.exec(
           "SELECT area::text AS area, coalesce(category, '') AS cat, source, "
           "  extract(epoch FROM started_at)::bigint AS f, extract(epoch FROM ended_at)::bigint AS t "
           "FROM detections WHERE status IN ('detected', 'confirmed') AND confidence >= 0.4 "
           "  AND started_at < to_timestamp($2::bigint) AND ended_at > to_timestamp($1::bigint)",
           from, to)) {
    const auto area = r["area"].as<std::string>();
    if (dropWorkDetections && area == "work") continue;
    const auto src = r["source"].as<std::string>();
    const int prec = src == "user" ? 0 : src == "vision" ? 1 : src == "process" ? 2 : 3;
    in.evidence.push_back({r["f"].as<int64_t>(), r["t"].as<int64_t>(), area, r["cat"].as<std::string>(), "", "", prec});
  }
  for (const auto& r : co_await tx.exec(
           "SELECT extract(epoch FROM starts_at)::bigint AS f, extract(epoch FROM ends_at)::bigint AS t "
           "FROM entertainment_slots WHERE deleted_at IS NULL AND consumed "
           "  AND starts_at < to_timestamp($2::bigint) AND ends_at > to_timestamp($1::bigint)",
           from, to)) {
    in.evidence.push_back({r["f"].as<int64_t>(), r["t"].as<int64_t>(), "entertainment", "", "", "", 0});
  }

  // ---- observed at all? (§4.6) evidence, progress logged, or the user talked to the assistant
  if (in.evidence.empty() && !progressLogged) {
    const auto talk = co_await tx.exec(
        "SELECT EXISTS (SELECT 1 FROM messages m JOIN conversations c ON c.id = m.conversation_id "
        "  WHERE c.user_id = $1::uuid AND c.local_day = $2::date AND m.role = 'user') AS t",
        userId, localDay);
    in.observed = talk[0]["t"].as<bool>();
  }

  // ---- targets (§4.2) and the work contexts' daily targets (§5)
  for (const auto& r : co_await tx.exec(
           "SELECT id::text AS id, title, area, metric, amount::float8 AS amount, day_profiles::text AS dp, "
           "  coalesce(match->>'category', '') AS cat, coalesce(match->>'link_type', '') AS lt, "
           "  coalesce(match->>'link_id', '') AS li, track_consistency FROM targets "
           "WHERE deleted_at IS NULL AND period = 'day' "
           "  AND (active_from IS NULL OR active_from <= $1::date) AND (active_to IS NULL OR active_to >= $1::date)",
           localDay)) {
    if (!appliesTo(textArray(r["dp"].as<std::string>()), profile)) continue;
    TargetInput t;
    t.id = r["id"].as<std::string>(), t.title = r["title"].as<std::string>(), t.area = r["area"].as<std::string>();
    t.metric = r["metric"].as<std::string>(), t.amount = r["amount"].as<double>();
    t.category = r["cat"].as<std::string>(), t.linkType = r["lt"].as<std::string>(), t.linkId = r["li"].as<std::string>();
    t.trackConsistency = r["track_consistency"].as<bool>();
    if (t.metric != "minutes") {
      // Counted from the records of the day; a skill_resource link narrows to that resource.
      const std::string res = t.linkType == "skill_resource" ? t.linkId : "";
      const auto c = co_await tx.exec(
          "SELECT "
          "  (SELECT coalesce(sum(greatest(page_to - page_from, 0)), 0) FROM skill_sessions s WHERE s.deleted_at IS NULL "
          "     AND s.started_at >= to_timestamp($1::bigint) AND s.started_at < to_timestamp($2::bigint) "
          "     AND ($3 = '' OR s.resource_id::text = $3))::float8 AS pages, "
          "  (SELECT count(DISTINCT section_id) FROM skill_sessions s WHERE s.deleted_at IS NULL AND section_id IS NOT NULL "
          "     AND s.started_at >= to_timestamp($1::bigint) AND s.started_at < to_timestamp($2::bigint) "
          "     AND ($3 = '' OR s.resource_id::text = $3))::float8 AS sections, "
          "  (SELECT count(*) FROM skill_sessions s WHERE s.deleted_at IS NULL AND $4 = 'skill' "
          "     AND s.started_at >= to_timestamp($1::bigint) AND s.started_at < to_timestamp($2::bigint) "
          "     AND ($3 = '' OR s.resource_id::text = $3))::float8 AS sessions, "
          "  (SELECT count(*) FROM activities a WHERE a.deleted_at IS NULL AND a.area::text = $4 "
          "     AND ($5 = '' OR a.category = $5) "
          "     AND a.started_at >= to_timestamp($1::bigint) AND a.started_at < to_timestamp($2::bigint))::float8 AS acts",
          from, to, res, t.area, t.category);
      const auto& row = c[0];
      if (t.metric == "pages") t.achieved = row["pages"].as<double>();
      else if (t.metric == "sections") t.achieved = row["sections"].as<double>();
      else if (t.metric == "sessions") t.achieved = row["sessions"].as<double>() + row["acts"].as<double>();
      else t.achieved = row["acts"].as<double>();
    }
    if (t.trackConsistency) {
      // Met on the previous applicable days, from their scores (unobserved and unscored days are skipped).
      const auto hist = co_await tx.exec(
          "SELECT count(*)::int AS n, count(*) FILTER (WHERE (t->>'ratio')::float8 >= 1)::int AS met "
          "FROM scores s, jsonb_array_elements(s.breakdown->'areas'->$2->'targets') t "
          "WHERE s.user_id = $1::uuid AND s.period = 'day' AND s.status IN ('final', 'provisional') "
          "  AND s.period_start BETWEEN $3::date - 6 AND $3::date - 1 AND t->>'id' = $4",
          userId, t.area, localDay, t.id);
      t.priorApplicable = hist[0]["n"].as<int>();
      t.priorMet = hist[0]["met"].as<int>();
    }
    in.targets.push_back(t);
  }
  if (profile.name == "workday") {
    for (const auto& c : contexts) {
      if (c["workday_target_min"].as<int>() <= 0) continue;
      TargetInput t;
      t.id = c["id"].as<std::string>(), t.title = "Workday (" + c["name"].as<std::string>() + ")";
      t.area = "work", t.metric = "minutes", t.amount = c["workday_target_min"].as<int>();
      in.targets.push_back(t);
    }
  }

  // ---- consistency of tracked routines over the trailing 7 applicable days (§4.4)
  const auto tracked = co_await tx.exec(
      "SELECT id::text AS id, title, day_profiles::text AS dp FROM tasks "
      "WHERE deleted_at IS NULL AND rrule IS NOT NULL AND track_consistency AND status = 'open'");
  if (!tracked.empty()) {
    Json::Value week;
    week["from"] = w[0]["ws"].as<std::string>();
    week["to"] = w[0]["ts"].as<std::string>();
    const auto span = co_await tools::registry().runInTx("calendar.view", sys, tx, week);
    // Days that do not count: unscored profiles and unobserved days.
    std::map<std::string, Profile> profiles;
    const auto skipped = co_await tx.exec(
        "SELECT to_char(period_start, 'YYYY-MM-DD') AS d FROM scores WHERE user_id = $1::uuid AND period = 'day' "
        "  AND status IN ('unobserved', 'not_scored') AND period_start BETWEEN $2::date - 6 AND $2::date - 1",
        userId, localDay);
    std::set<std::string> skipDays;
    for (const auto& s : skipped) skipDays.insert(s["d"].as<std::string>());
    for (const auto& r : tracked) {
      TrackedRoutine rt;
      rt.id = r["id"].as<std::string>(), rt.title = r["title"].as<std::string>();
      const auto dp = textArray(r["dp"].as<std::string>());
      for (const auto& e : span["items"]) {
        if (e["kind"].asString() != "task_occurrence" || e["id"].asString() != rt.id) continue;
        const auto d = e["occurs_on"].asString();
        if (d.empty() || d > localDay || skipDays.count(d)) continue;
        if (!profiles.count(d)) profiles[d] = co_await profileFor(tx, userId, d);
        if (!profiles[d].scored || !appliesTo(dp, profiles[d])) continue;
        ++rt.applicable;
        if (e["status"].asString() == "done") ++rt.completed;
      }
      if (rt.applicable > 0) in.routines.push_back(rt);
    }
  }
  co_return in;
}

}  // namespace thelife::scoring
