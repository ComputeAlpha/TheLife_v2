#include "proactive/notifier.h"

#include "agent/events.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/job_store.h"
#include "telegram/bot.h"

namespace thelife::proactive {

drogon::Task<Gate> gate(db::TenantTx& tx, const std::string& userId, bool urgent) {
  const auto r = co_await tx.exec(
      "WITH u AS (SELECT timezone, day_start_hour, (now() AT TIME ZONE timezone) AS lt FROM users WHERE id = $1::uuid) "
      "SELECT (u.lt::time >= '22:30' OR u.lt::time < '07:30') AS quiet, "
      "  EXISTS (SELECT 1 FROM messages m JOIN conversations c ON c.id = m.conversation_id "
      "          WHERE c.user_id = $1::uuid AND m.role = 'user' AND m.created_at > now() - interval '10 minutes') "
      "  OR EXISTS (SELECT 1 FROM activities a WHERE a.deleted_at IS NULL AND a.created_at > now() - interval '10 minutes') "
      "  AS active, "
      "  (SELECT count(*) FROM notifications n WHERE n.user_id = $1::uuid AND n.proactive "
      "     AND n.created_at >= (((u.lt - make_interval(hours => u.day_start_hour))::date "
      "                          + make_interval(hours => u.day_start_hour)) AT TIME ZONE u.timezone))::int AS today, "
      "  coalesce(extract(epoch FROM now() - (SELECT max(created_at) FROM notifications n "
      "     WHERE n.user_id = $1::uuid AND n.proactive)) / 60, 1e9)::float8 AS since "
      "FROM u",
      userId);
  Gate g;
  if (r.empty()) co_return g;
  if (r[0]["quiet"].as<bool>() && !r[0]["active"].as<bool>()) {
    g.deliver = false, g.reason = "quiet_hours";
  } else if (r[0]["today"].as<int>() >= kDailyCap) {
    g.deliver = false, g.reason = "daily_cap";
  } else if (!urgent && r[0]["since"].as<double>() < kSpacingMinutes) {
    g.deliver = false, g.reason = "spacing";
  }
  co_return g;
}

drogon::Task<std::string> notify(db::TenantTx& tx, const Notice& n) {
  const auto id = ids::newUuidV7();
  std::string title = n.title.substr(0, 80);
  std::string body = n.body.size() > 400 ? n.body.substr(0, 397) + "..." : n.body;
  std::vector<std::optional<std::string>> params{
      id, tx.tenantId(), n.userId, n.kind, n.urgency, title, body, n.action, n.refType, n.refId, n.conversationId,
      n.messageId, n.proactive ? "true" : "false"};
  co_await tx.execParams(
      "INSERT INTO notifications (id, tenant_id, user_id, kind, urgency, title, body, action, ref_type, ref_id, "
      "conversation_id, message_id, proactive, expires_at) VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6, $7, $8, "
      "$9, $10::uuid, $11::uuid, $12::uuid, $13::boolean, now() + interval '24 hours')",
      std::move(params));
  // Browser: every open web tab holding the stream (§8 rule 2). The SSE event carries the notification.
  const bool browser = agent::EventHub::instance().subscribers(n.userId) > 0;
  if (browser) {
    co_await tx.exec(
        "INSERT INTO notification_deliveries (id, tenant_id, notification_id, channel, status) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'browser', 'sent')",
        ids::newUuidV7(), tx.tenantId(), id);
    Json::Value e;
    e["id"] = id, e["kind"] = n.kind, e["urgency"] = n.urgency, e["title"] = title, e["body"] = body;
    agent::EventHub::instance().publish(n.userId, "notification", e);
  }
  // Telegram (§8 rule 3): immediately when away, otherwise as the fallback when nothing acked it in 3 minutes.
  if (!n.telegramSent && co_await telegram::chatOf(tx, n.userId)) {
    const auto seen = co_await tx.exec(
        "SELECT EXISTS (SELECT 1 FROM devices WHERE user_id = $1::uuid AND revoked_at IS NULL AND deleted_at IS NULL "
        "               AND last_seen_at > now() - interval '2 minutes') AS fresh",
        n.userId);
    const bool away = !browser && !seen[0]["fresh"].as<bool>();
    co_await tx.exec(
        "INSERT INTO notification_deliveries (id, tenant_id, notification_id, channel, status) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'telegram', 'queued')",
        ids::newUuidV7(), tx.tenantId(), id);
    db::jobs::NewJob j;
    j.kind = "notify.telegram";
    j.payload["notification_id"] = id;
    j.delaySec = away ? 0 : kTelegramFallbackSeconds;
    j.priority = 1;
    co_await db::jobs::enqueue(tx, std::move(j));
  }
  co_return id;
}

drogon::Task<void> emit(db::TenantTx& tx, std::optional<std::string> userId, const std::string& kind,
                        const std::string& entityType, std::optional<std::string> entityId, Json::Value payload) {
  std::vector<std::optional<std::string>> params{ids::newUuidV7(), tx.tenantId(), userId, kind, entityType, entityId,
                                                 json::compact(payload)};
  co_await tx.execParams(
      "INSERT INTO events (id, tenant_id, user_id, kind, entity_type, entity_id, payload) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, $4, $5, $6::uuid, $7::jsonb)",
      std::move(params));
}

}  // namespace thelife::proactive
