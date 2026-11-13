// Reminders, routines, agent follow-ups and notifications (agent_lld.md §4, §7.4, §8).
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "crud/resource.h"
#include "modules/common.h"
#include "modules/modules.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

Task<Json::Value> skipOnce(ToolCall& c) {
  const auto id = c.args["id"].asString();
  std::vector<std::optional<std::string>> params{
      id, c.args.isMember("date") ? std::optional(c.args["date"].asString()) : std::nullopt, c.principal.userId};
  const auto r = co_await c.tx.execParams(
      "UPDATE routines SET skip_on = coalesce($2::date, (SELECT ((now() AT TIME ZONE timezone) "
      "  - make_interval(hours => day_start_hour))::date FROM users WHERE id = $3::uuid)) "
      "WHERE id = $1::uuid AND deleted_at IS NULL RETURNING id",
      std::move(params));
  if (r.empty()) throw ApiError::notFound();
  co_return co_await crud::fetchApi(c.tx, "routines", id, crud::projectionFor(tools::registry(), "routines"));
}

Task<Json::Value> scheduleFollowup(ToolCall& c) {
  const auto id = ids::newUuidV7();
  std::vector<std::optional<std::string>> params{id, c.principal.tenantId, c.principal.userId, c.conversationId,
                                                 c.args["at"].asString(), c.args["reason"].asString(), c.sourceMessageId};
  const auto r = co_await c.tx.execParams(
      "INSERT INTO agent_followups (id, tenant_id, created_by_id, user_id, conversation_id, channel, due_at, reason, "
      "source_message_id) SELECT $1::uuid, $2::uuid, $3::uuid, $3::uuid, $4::uuid, "
      "coalesce((SELECT channel FROM conversations WHERE id = $4::uuid), 'web'), $5::timestamptz, $6, $7::uuid "
      "WHERE $5::timestamptz > now() RETURNING to_jsonb(due_at) #>> '{}' AS due",
      std::move(params));
  if (r.empty()) throw ApiError::validation({{"/at", "must be in the future"}});
  Json::Value out;
  out["id"] = id;
  out["due_at"] = r[0]["due"].as<std::string>();
  co_return out;
}

Task<Json::Value> cancelFollowup(ToolCall& c) {
  const auto r = co_await c.tx.exec(
      "UPDATE agent_followups SET status = 'cancelled', deleted_at = now() WHERE id = $1::uuid AND status = 'pending' "
      "AND deleted_at IS NULL RETURNING id",
      c.args["id"].asString());
  if (r.empty()) throw ApiError::notFound();
  co_return Json::Value();
}

Task<Json::Value> ack(ToolCall& c) {
  const auto id = c.args["id"].asString();
  const auto n = co_await c.tx.exec("SELECT 1 FROM notifications WHERE id = $1::uuid AND user_id = $2::uuid", id,
                                    c.principal.userId);
  if (n.empty()) throw ApiError::notFound();
  co_await c.tx.exec(
      "INSERT INTO notification_deliveries (id, tenant_id, notification_id, channel, status) VALUES ($1::uuid, "
      "$2::uuid, $3::uuid, 'browser', $4) ON CONFLICT (notification_id, channel, device_id) DO UPDATE SET "
      "status = EXCLUDED.status, at = now()",
      ids::newUuidV7(), c.principal.tenantId, id, c.args.get("clicked", false).asBool() ? "clicked" : "shown");
  co_await c.tx.exec("UPDATE notifications SET resolved_at = coalesce(resolved_at, now()) WHERE id = $1::uuid", id);
  co_return co_await crud::fetchApi(c.tx, "notification_feed", id, crud::projectionFor(tools::registry(), "notifications"));
}

void validateReminder(Json::Value& f, bool) { requireValidRrule(f, "rrule"); }

void validateRoutine(Json::Value& f, bool) {
  const auto ws = f["window_start"], we = f["window_end"];
  if (ws.isString() != we.isString()) throw ApiError::validation({{"/window_end", "give both window_start and window_end"}});
  if (ws.isString() && ws.asString() >= we.asString()) {
    throw ApiError::validation({{"/window_end", "must be after window_start"}});
  }
}

}  // namespace

void bindProactive(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "reminders", .table = "reminders", .filters = {}, .validate = validateReminder});
  crud::bindCrud(reg, {.prefix = "routines", .table = "routines", .filters = {}, .validate = validateRoutine});
  reg.bind("routines.skip_once", skipOnce, crud::targetResolver("routines"));
  reg.bind("agent.schedule_followup", scheduleFollowup);
  reg.bind("agent.cancel_followup", cancelFollowup, crud::targetResolver("agent_followups"));
  // Handled by the agent loop (they mark the turn); bound so direct calls answer.
  reg.bind("agent.no_message", [](ToolCall&) -> Task<Json::Value> {
    Json::Value v;
    v["ok"] = true;
    co_return v;
  });
  reg.bind("notify.urgent", [](ToolCall&) -> Task<Json::Value> {
    Json::Value v;
    v["ok"] = true;
    co_return v;
  });
  crud::bindCrud(reg, {.prefix = "notifications",
                       .table = "notification_feed",
                       .filters = {{"open", "((r.resolved_at IS NULL) = $?::boolean)"}},
                       .validate = nullptr});
  reg.bind("notifications.ack", ack);
}

}  // namespace thelife::modules
