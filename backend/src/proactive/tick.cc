// proactive.tick (agent_lld.md §4): for one user, turn what became due into deliveries and agent turns.
//   outbox events → triggers (coalesced by dedupe key)          scans: deliverables due, entertainment while due
//   reminders → template delivery (no model)                     follow-ups and due triggers → proactive turns
//   routines → proactive turns at their (adaptive) time          deferred replies → answered once the model is back
#include "proactive/tick.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cstdlib>

#include "agent/events.h"
#include "agent/runner.h"
#include "agent/store.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/recurrence.h"
#include "jobs/worker.h"
#include "llm/gateway.h"
#include "modules/common.h"
#include "proactive/notifier.h"
#include "scoring/inputs.h"

namespace thelife::proactive {

namespace {

using drogon::Task;

std::string str(const drogon::orm::Field& f) { return f.isNull() ? std::string() : f.as<std::string>(); }

// Adds a trigger unless one with this dedupe key ever existed (a burst coalesces into one).
Task<void> addTrigger(db::TenantTx& tx, const std::string& user, const std::string& kind, const std::string& key,
                      const std::string& details, bool urgent = false, bool silent = false) {
  Json::Value p;
  p["details"] = details;
  p["urgent"] = urgent;
  p["silent"] = silent;
  co_await tx.exec(
      "INSERT INTO agent_triggers (id, tenant_id, user_id, kind, dedupe_key, payload) VALUES ($1::uuid, $2::uuid, "
      "$3::uuid, $4, $5, $6::jsonb) ON CONFLICT (tenant_id, user_id, dedupe_key) DO NOTHING",
      ids::newUuidV7(), tx.tenantId(), user, kind, key, json::compact(p));
}

Task<void> eventsToTriggers(db::TenantTx& tx, const std::string& user) {
  const auto rows = co_await tx.exec(
      "SELECT id::text AS id, kind, coalesce(entity_id::text, '') AS entity, payload::text AS payload FROM events "
      "WHERE processed_at IS NULL AND (user_id IS NULL OR user_id = $1::uuid) ORDER BY created_at LIMIT 200 "
      "FOR UPDATE SKIP LOCKED",
      user);
  for (const auto& e : rows) {
    const auto kind = e["kind"].as<std::string>();
    const auto p = json::parse(e["payload"].as<std::string>());
    if (kind == "task.missed") {
      std::string list;
      for (const auto& t : p["titles"]) list += "- " + t.asString() + "\n";
      co_await addTrigger(tx, user, "tasks_missed", "tasks_missed:" + p["day"].asString(),
                          "Day " + p["day"].asString() + ", not done:\n" + list);
    } else if (kind == "detection.disputed") {
      co_await addTrigger(tx, user, "dispute_review", "dispute:" + e["entity"].as<std::string>(),
                          "Detection " + e["entity"].as<std::string>() + " (" + p["category"].asString() +
                              "; detections.get shows it). The user's reason: " + p["reason"].asString(),
                          false, /*silent=*/true);
    } else if (kind == "day.unobserved") {
      co_await addTrigger(tx, user, "day_unobserved", "unobserved:" + p["day"].asString(),
                          "Day without any signal: " + p["day"].asString() + ".");
    }
    co_await tx.exec("UPDATE events SET processed_at = now() WHERE id = $1::uuid", e["id"].as<std::string>());
  }
}

// Scans for situations no single change announces (§4.1, scoring_lld.md §6.1).
Task<void> scans(db::TenantTx& tx, const std::string& user, const agent::NowInfo& now) {
  for (const auto& d : co_await tx.exec(
           "SELECT d.id::text AS id, d.title, to_char(d.due_at AT TIME ZONE u.timezone, 'Dy HH24:MI') AS due, "
           "  d.due_at <= now() + interval '2 hours' AS soon, to_char(d.due_at, 'YYYYMMDDHH24MI') AS k "
           "FROM work_deliverables d, users u WHERE u.id = $1::uuid AND d.deleted_at IS NULL "
           "  AND d.status NOT IN ('delivered', 'cancelled') AND d.due_at > now() AND d.due_at <= now() + interval '24 hours'",
           user)) {
    const auto id = d["id"].as<std::string>(), k = d["k"].as<std::string>();
    const auto details = "Deliverable \"" + d["title"].as<std::string>() + "\" is due " + d["due"].as<std::string>() + ".";
    co_await addTrigger(tx, user, "deliverable_due", "deliverable24:" + id + ":" + k, details);
    if (d["soon"].as<bool>()) co_await addTrigger(tx, user, "deliverable_due", "deliverable2:" + id + ":" + k, details, true);
  }

  // Entertainment for 15+ minutes up to now (still running), while a block is on or something is due.
  const auto ent = co_await tx.exec(
      "SELECT min(s)::text AS started, extract(epoch FROM min(s))::bigint AS k FROM ("
      "  SELECT started_at AS s, ended_at AS e FROM activities WHERE deleted_at IS NULL AND area = 'entertainment' "
      "  UNION ALL SELECT started_at, ended_at FROM detections WHERE area = 'entertainment' "
      "    AND status IN ('detected', 'confirmed')) x "
      "WHERE e >= now() - interval '5 minutes' AND s <= now() - interval '15 minutes'");
  if (!ent.empty() && !ent[0]["started"].isNull()) {
    std::string due;
    for (const auto& b : co_await tx.exec(
             "SELECT title FROM calendar_events WHERE deleted_at IS NULL AND NOT all_day AND rrule IS NULL "
             "  AND area IN ('work', 'skill', 'fitness', 'personal') AND starts_at <= now() AND ends_at > now()")) {
      due += "- planned now: " + b["title"].as<std::string>() + "\n";
    }
    for (const auto& t : co_await tx.exec(
             "SELECT title FROM tasks WHERE deleted_at IS NULL AND status = 'open' AND rrule IS NULL "
             "  AND due_at < $1::timestamptz LIMIT 5",
             now.dayTo)) {
      due += "- due today or overdue: " + t["title"].as<std::string>() + "\n";
    }
    if (!due.empty()) {
      co_await addTrigger(tx, user, "entertainment_warning", "ent:" + ent[0]["k"].as<std::string>(),
                          "Entertainment since " + ent[0]["started"].as<std::string>() + ".\n" + due);
    }
  }
}

// Due reminders: a template message and a notification, no model (§7.4); repeating ones move to the next time.
Task<void> reminders(db::TenantTx& tx, const std::string& user) {
  const auto rows = co_await tx.exec(
      "SELECT r.id::text AS id, r.text, r.rrule, r.link_type, r.link_id::text AS link_id, "
      "  to_char(r.due_at AT TIME ZONE u.timezone, 'YYYY-MM-DD HH24:MI:SS') AS local, u.timezone "
      "FROM reminders r, users u WHERE u.id = $1::uuid AND r.deleted_at IS NULL AND r.status = 'pending' "
      "  AND r.due_at <= now() ORDER BY r.due_at LIMIT 50 FOR UPDATE OF r SKIP LOCKED",
      user);
  for (const auto& r : rows) {
    const auto id = r["id"].as<std::string>();
    if (str(r["link_type"]) == "task") {
      const auto t = co_await tx.exec("SELECT status FROM tasks WHERE id = $1::uuid", str(r["link_id"]));
      if (!t.empty() && t[0]["status"].as<std::string>() == "done") {  // already done: drop it quietly
        co_await tx.exec("UPDATE reminders SET status = 'cancelled' WHERE id = $1::uuid", id);
        continue;
      }
    }
    const auto conv = co_await agent::ensureConversation(tx, user, "web");
    agent::StoredMessage m;
    m.conversationId = conv.id;
    m.role = "assistant";
    m.kind = "deterministic";
    m.content = "Reminder: " + r["text"].as<std::string>();
    const auto messageId = co_await agent::insertMessage(tx, m);
    Notice n;
    n.userId = user, n.kind = "reminder", n.title = "Reminder", n.body = r["text"].as<std::string>();
    n.refType = "reminder", n.refId = id, n.conversationId = conv.id, n.messageId = messageId;
    co_await notify(tx, n);
    Json::Value e;
    e["message_id"] = messageId, e["kind"] = "deterministic", e["text"] = m.content;
    agent::EventHub::instance().publish(user, "message", e);

    std::optional<std::string> next;
    if (!str(r["rrule"]).empty()) {
      const auto start = recurrence::LocalDateTime::parse(r["local"].as<std::string>());
      if (start) {
        const auto occ = recurrence::expand(r["rrule"].as<std::string>(), *start, start->addSeconds(60),
                                            start->addDays(800), 1);
        if (!occ.empty()) {
          std::vector<std::pair<recurrence::LocalDateTime, recurrence::LocalDateTime>> span;
          span.emplace_back(occ[0], occ[0]);
          const auto inst = co_await modules::toInstants(tx, r["timezone"].as<std::string>(), span);
          if (!inst.empty()) next = inst[0].first;
        }
      }
    }
    if (next) {
      co_await tx.exec("UPDATE reminders SET due_at = $2::timestamptz, last_sent_at = now() WHERE id = $1::uuid", id, *next);
    } else {
      co_await tx.exec("UPDATE reminders SET status = 'sent', last_sent_at = now() WHERE id = $1::uuid", id);
    }
  }
}

std::vector<std::string> textArray(const std::string& pg) {
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

int minutesOf(const std::string& hhmm) {
  return hhmm.size() >= 5 ? std::atoi(hhmm.substr(0, 2).c_str()) * 60 + std::atoi(hhmm.substr(3, 2).c_str()) : 0;
}

// Routines (§4.4): explicit calendar override, else fixed time, else adaptive (morning: the first activity within
// the window, or the learned/default time; evening: the learned/default time). A routine whose moment passed
// long ago today is dropped for the day rather than sent late.
Task<std::vector<agent::TurnRequest>> routines(db::TenantTx& tx, const std::string& user, const agent::NowInfo& now) {
  std::vector<agent::TurnRequest> out;
  const auto rows = co_await tx.exec(
      "SELECT id::text AS id, kind, title, mode, default_time, coalesce(window_start, '') AS ws, "
      "  coalesce(window_end, '') AS we, weekdays::text AS wd, day_profiles::text AS dp, coalesce(learned_time, '') AS lt, "
      "  coalesce(instructions, '') AS ins, coalesce(skip_on = $1::date, false) AS skip FROM routines "
      "WHERE deleted_at IS NULL AND enabled AND (last_fired_on IS NULL OR last_fired_on < $1::date) "
      "FOR UPDATE SKIP LOCKED",
      now.localDay);
  if (rows.empty()) co_return out;
  const auto clock = co_await tx.exec(
      "SELECT to_char(now() AT TIME ZONE u.timezone, 'HH24:MI') AS t, upper(left(to_char($2::date, 'DY'), 2)) AS dow, "
      "  to_char(least("
      "    (SELECT min(m.created_at) FROM messages m JOIN conversations c ON c.id = m.conversation_id "
      "      WHERE c.user_id = $1::uuid AND m.role = 'user' AND m.created_at >= $3::timestamptz), "
      "    (SELECT min(created_at) FROM activities WHERE created_at >= $3::timestamptz), "
      "    (SELECT min(started_at) FROM detections WHERE started_at >= $3::timestamptz)) AT TIME ZONE u.timezone, "
      "    'HH24:MI') AS first "
      "FROM users u WHERE u.id = $1::uuid",
      user, now.localDay, now.dayFrom);
  const auto nowT = clock[0]["t"].as<std::string>();
  const auto dow = clock[0]["dow"].as<std::string>();
  const auto first = str(clock[0]["first"]);
  const auto profile = co_await scoring::profileFor(tx, user, now.localDay);
  const auto quiet = (co_await gate(tx, user, true)).reason == "quiet_hours";

  for (const auto& r : rows) {
    const auto id = r["id"].as<std::string>(), kind = r["kind"].as<std::string>();
    const auto weekdays = textArray(r["wd"].as<std::string>()), profiles = textArray(r["dp"].as<std::string>());
    auto markFired = [&]() -> Task<void> {
      co_await tx.exec("UPDATE routines SET last_fired_on = $2::date WHERE id = $1::uuid", id, now.localDay);
    };
    const bool notToday = (!weekdays.empty() && std::find(weekdays.begin(), weekdays.end(), dow) == weekdays.end()) ||
                          (!profiles.empty() && std::find(profiles.begin(), profiles.end(), profile.name) == profiles.end()) ||
                          r["skip"].as<bool>();
    if (notToday) {
      co_await markFired();
      continue;
    }
    const auto override = co_await tx.exec(
        "SELECT to_char(e.starts_at AT TIME ZONE u.timezone, 'HH24:MI') AS t FROM calendar_events e, users u "
        "WHERE u.id = $1::uuid AND e.deleted_at IS NULL AND NOT e.all_day AND e.rrule IS NULL AND e.category = $2 "
        "  AND e.starts_at >= $3::timestamptz AND e.starts_at < $4::timestamptz ORDER BY e.starts_at LIMIT 1",
        user, "routine." + kind, now.dayFrom, now.dayTo);
    const auto ws = r["ws"].as<std::string>(), we = r["we"].as<std::string>();
    const auto planned = !override.empty() ? override[0]["t"].as<std::string>()
                         : r["mode"].as<std::string>() == "fixed" || r["lt"].as<std::string>().empty()
                             ? r["default_time"].as<std::string>()
                             : r["lt"].as<std::string>();
    bool due = nowT >= planned;
    if (override.empty() && r["mode"].as<std::string>() == "adaptive" && kind == "morning" && !first.empty() &&
        !ws.empty() && nowT >= ws && first >= ws) {
      due = true;  // the user has started their day inside the window
    }
    if (!due) continue;
    // Too late to be useful today: past the window's end, or 3 h after a time without a window.
    const int lateLimit = !we.empty() && override.empty() ? minutesOf(we) : minutesOf(planned) + 180;
    if (minutesOf(nowT) > lateLimit) {
      co_await markFired();
      continue;
    }
    if (quiet) continue;  // wait until the user is up or quiet hours end
    co_await markFired();
    agent::TurnRequest t;
    t.trigger = "proactive";
    t.triggerKind = "routine." + kind;
    t.details = "Routine: " + r["title"].as<std::string>() + "." +
                (r["ins"].as<std::string>().empty() ? "" : " Instructions: " + r["ins"].as<std::string>());
    out.push_back(t);
  }
  co_return out;
}

Task<void> tick(const jobs::Job& job) {
  const auto user = job.payload["user_id"].asString();
  std::vector<agent::TurnRequest> turns;
  {
    auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
    const auto now = co_await agent::loadNow(tx, user);
    co_await eventsToTriggers(tx, user);
    co_await scans(tx, user, now);
    co_await reminders(tx, user);

    for (const auto& f : co_await tx.exec(
             "UPDATE agent_followups SET status = 'fired', fired_at = now() WHERE user_id = $1::uuid AND status = 'pending' "
             "AND deleted_at IS NULL AND due_at <= now() RETURNING reason, channel",
             user)) {
      agent::TurnRequest t;
      t.trigger = "proactive", t.triggerKind = "followup", t.details = f["reason"].as<std::string>();
      t.channel = f["channel"].as<std::string>();
      turns.push_back(t);
    }
    for (const auto& g : co_await tx.exec(
             "UPDATE agent_triggers SET status = 'fired', fired_at = now() WHERE user_id = $1::uuid AND status = 'pending' "
             "AND due_at <= now() RETURNING kind, payload::text AS payload",
             user)) {
      const auto p = json::parse(g["payload"].as<std::string>());
      agent::TurnRequest t;
      t.trigger = "proactive", t.triggerKind = g["kind"].as<std::string>(), t.details = p["details"].asString();
      t.urgent = p["urgent"].asBool(), t.silent = p["silent"].asBool();
      turns.push_back(t);
    }
    for (auto& t : co_await routines(tx, user, now)) turns.push_back(std::move(t));

    // Replies deferred while the assistant was offline (agent_lld.md §3.4 catch-up), first in the queue.
    if (llm::gateway().mode() == "online") {
      std::vector<agent::TurnRequest> deferred;
      for (const auto& m : co_await tx.exec(
               "SELECT DISTINCT ON (t.message_id) t.message_id::text AS msg, t.conversation_id::text AS conv, "
               "  c.channel FROM turns t JOIN messages m ON m.id = t.message_id "
               "JOIN conversations c ON c.id = t.conversation_id "
               "WHERE t.user_id = $1::uuid AND t.status = 'manual' AND t.trigger = 'user_message' "
               "  AND t.created_at > now() - interval '7 days' AND left(m.content, 1) <> '/' "
               "  AND NOT EXISTS (SELECT 1 FROM turns d WHERE d.message_id = t.message_id AND d.trigger = 'deferred_reply') "
               "ORDER BY t.message_id",
               user)) {
        const auto id = ids::newUuidV7();
        co_await tx.exec(
            "INSERT INTO turns (id, tenant_id, user_id, conversation_id, message_id, trigger) "
            "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::uuid, 'deferred_reply')",
            id, job.tenantId, user, m["conv"].as<std::string>(), m["msg"].as<std::string>());
        agent::TurnRequest t;
        t.trigger = "deferred_reply", t.turnId = id, t.channel = m["channel"].as<std::string>();
        deferred.push_back(t);
      }
      turns.insert(turns.begin(), deferred.begin(), deferred.end());
    }
    // Proactive turns are recorded (queued) in this transaction, so none is lost between here and the queue.
    for (auto& t : turns) {
      if (t.trigger != "proactive") continue;
      const auto conv = co_await agent::ensureConversation(tx, user, t.channel);
      t.turnId = ids::newUuidV7();
      co_await tx.exec(
          "INSERT INTO turns (id, tenant_id, user_id, conversation_id, trigger, trigger_kind) VALUES ($1::uuid, "
          "$2::uuid, $3::uuid, $4::uuid, 'proactive', $5)",
          t.turnId, job.tenantId, user, conv.id, t.triggerKind);
    }
    co_await tx.commit();
  }
  for (auto& t : turns) {
    t.tenantId = job.tenantId;
    t.userId = user;
    agent::submitTurn(t);
  }
}

}  // namespace

void registerTick() { jobs::registerHandler("proactive.tick", tick); }

}  // namespace thelife::proactive
