// Desktop notification delivery to the collector (agent_lld.md §8, collector_lld.md §7.5). The Notifier creates
// notifications; here the device fetches the ones not yet shown on it and acks them. An ack on any channel
// resolves the notification everywhere.
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "modules/modules.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

Task<Json::Value> poll(ToolCall& c) {
  // `wait` (long poll) is handled by the REST adapter, which repeats this call in short transactions.
  const auto rows = co_await c.tx.exec(
      "SELECT jsonb_build_object('id', n.id, 'title', n.title, 'body', n.body, 'urgency', n.urgency, "
      "  'action', n.action, 'ref_id', n.ref_id)::text AS j, n.id::text AS id "
      "FROM notifications n WHERE n.user_id = $1::uuid AND n.resolved_at IS NULL "
      "  AND (n.expires_at IS NULL OR n.expires_at > now()) "
      "  AND NOT EXISTS (SELECT 1 FROM notification_deliveries d WHERE d.notification_id = n.id "
      "                  AND d.channel = 'desktop' AND d.device_id = $2::uuid AND d.status IN ('shown', 'clicked')) "
      "ORDER BY n.created_at LIMIT 20",
      c.principal.userId, c.principal.deviceId);
  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  for (const auto& r : rows) {
    out["items"].append(json::parse(r["j"].as<std::string>()));
    co_await c.tx.exec(
        "INSERT INTO notification_deliveries (id, tenant_id, notification_id, channel, device_id, status) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'desktop', $4::uuid, 'sent') "
        "ON CONFLICT (notification_id, channel, device_id) DO NOTHING",
        ids::newUuidV7(), c.principal.tenantId, r["id"].as<std::string>(), c.principal.deviceId);
  }
  co_return out;
}

Task<Json::Value> ack(ToolCall& c) {
  const auto id = c.args["id"].asString();
  const auto n = co_await c.tx.exec("SELECT 1 FROM notifications WHERE id = $1::uuid AND user_id = $2::uuid", id,
                                    c.principal.userId);
  if (n.empty()) throw ApiError::notFound();
  const std::string status = c.args["clicked"].asBool() ? "clicked" : "shown";
  co_await c.tx.exec(
      "INSERT INTO notification_deliveries (id, tenant_id, notification_id, channel, device_id, status, at) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, 'desktop', $4::uuid, $5, $6::timestamptz) "
      "ON CONFLICT (notification_id, channel, device_id) DO UPDATE SET status = EXCLUDED.status, at = EXCLUDED.at",
      ids::newUuidV7(), c.principal.tenantId, id, c.principal.deviceId, status, c.args["shown_at"].asString());
  co_await c.tx.exec("UPDATE notifications SET resolved_at = coalesce(resolved_at, now()) WHERE id = $1::uuid", id);
  co_return Json::Value();
}

}  // namespace

void bindNotifications(tools::ToolRegistry& reg) {
  reg.bind("devices.self_notifications", poll);
  reg.bind("devices.self_ack_notification", ack);
}

}  // namespace thelife::modules
