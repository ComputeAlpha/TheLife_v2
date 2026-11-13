// Approvals, conversations and the agent-only tools (agent_lld.md §5, §7).
#include "agent/runner.h"
#include "agent/store.h"
#include "core/errors.h"
#include "core/json.h"
#include "crud/resource.h"
#include "modules/modules.h"
#include "scoring/engine.h"
#include "scoring/inputs.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

// Approve runs the stored call exactly, as the agent, pinned to the version the agent saw. It runs inside a
// savepoint of the approval's own transaction, so the decision and its effect commit together.
Task<Json::Value> decide(ToolCall& c, bool approve) {
  const auto id = c.args["id"].asString();
  const auto rows = co_await c.tx.exec(
      "SELECT a.tool, a.args::text AS args, a.status, (a.status = 'pending' AND a.expires_at <= now()) AS expired, "
      "  a.target_version, coalesce(cv.channel, 'web') AS channel, a.origin::text AS origin, "
      "  a.source_message_id::text AS source "
      "FROM approval_requests a LEFT JOIN conversations cv ON cv.id = a.conversation_id "
      "WHERE a.id = $1::uuid AND a.user_id = $2::uuid AND a.deleted_at IS NULL FOR UPDATE OF a",
      id, c.principal.userId);
  if (rows.empty()) throw ApiError::notFound();
  const auto& row = rows[0];
  if (row["expired"].as<bool>()) throw ApiError::conflict("This approval expired; ask the assistant again if still wanted.");
  if (row["status"].as<std::string>() != "pending") {
    throw ApiError::conflict("This approval was already " + row["status"].as<std::string>() + ".");
  }
  const auto tool = row["tool"].as<std::string>();
  std::string status = "rejected";
  std::optional<std::string> error, result;
  if (approve) {
    auth::Principal agentP;
    agentP.kind = auth::PrincipalKind::Agent;
    agentP.tenantId = c.principal.tenantId;
    agentP.userId = c.principal.userId;
    auto args = json::parse(row["args"].as<std::string>());
    if (!row["target_version"].isNull()) args["expected_version"] = row["target_version"].as<int>();
    co_await c.tx.exec("SAVEPOINT approval_exec");
    std::optional<ApiError> failed;
    Json::Value value;
    try {
      // Rows the approved call creates keep the provenance of the request (user_stated when the user asked).
      value = co_await tools::registry().runInTx(
          tool, agentP, c.tx, args, row["origin"].as<std::string>(),
          row["source"].isNull() ? std::nullopt : std::optional<std::string>(row["source"].as<std::string>()));
    } catch (const ApiError& e) {
      failed = e;
    }
    if (failed) {
      co_await c.tx.exec("ROLLBACK TO SAVEPOINT approval_exec");
      // The target changed (412) or vanished (404) since the agent looked: stale, the agent re-proposes.
      status = failed->status() == 412 || failed->status() == 404 ? "stale" : "failed";
      error = failed->title() + (failed->detail().empty() ? "" : ": " + failed->detail());
    } else {
      co_await c.tx.exec("RELEASE SAVEPOINT approval_exec");
      status = "approved";
      result = json::compact(value.isNull() ? Json::Value(Json::objectValue) : value);
    }
  }
  std::vector<std::optional<std::string>> params{
      id, status, c.principal.userId,
      c.args.isMember("reason") ? std::optional<std::string>(c.args["reason"].asString()) : std::nullopt, result,
      error};
  co_await c.tx.execParams(
      "UPDATE approval_requests SET status = $2, decided_at = now(), decided_by = $3::uuid, decision_reason = $4, "
      "result = $5::jsonb, error = $6 WHERE id = $1::uuid",
      std::move(params));

  agent::AuditEntry a;
  a.userId = c.principal.userId;
  a.tool = tool;
  a.args = json::parse(row["args"].as<std::string>());
  a.decision = approve ? "approved" : "deny";
  a.outcome = !approve ? "denied" : status == "approved" ? "executed" : "failed";
  a.approvalId = id;
  a.error = error;
  co_await agent::insertAudit(c.tx, a);

  // The agent hears about the decision in the channel the request came from (§4.1).
  agent::TurnRequest turn;
  turn.tenantId = c.principal.tenantId;
  turn.userId = c.principal.userId;
  turn.channel = row["channel"].as<std::string>();
  turn.trigger = "approval";
  turn.approvalId = id;
  c.afterCommit.push_back([turn] { agent::submitTurn(turn); });

  co_return co_await crud::fetchApi(c.tx, "approvals", id, crud::projectionFor(tools::registry(), "approvals"));
}

Task<Json::Value> conversationMessages(ToolCall& c) {
  const auto id = c.args["id"].asString();
  const auto conv = co_await c.tx.exec("SELECT 1 FROM conversations WHERE id = $1::uuid AND deleted_at IS NULL", id);
  if (conv.empty()) throw ApiError::notFound();
  std::vector<std::optional<std::string>> params{
      id, c.args.isMember("after") ? std::optional<std::string>(c.args["after"].asString()) : std::nullopt,
      std::to_string(c.args.get("limit", 200).asInt())};
  const auto rows = co_await c.tx.execParams(
      "SELECT jsonb_build_object('id', m.id, 'conversation_id', m.conversation_id, 'turn_id', m.turn_id, "
      "  'role', m.role, 'kind', m.kind, 'content', m.content, 'tool_calls', m.tool_calls, "
      "  'tool_call_id', m.tool_call_id, 'tool_name', m.tool_name, 'created_at', m.created_at)::text AS j "
      "FROM messages m WHERE m.conversation_id = $1::uuid AND m.kind <> 'rolling_summary' "
      "  AND ($2::uuid IS NULL OR (m.created_at, m.id) > "
      "       (SELECT a.created_at, a.id FROM messages a WHERE a.id = $2::uuid AND a.conversation_id = $1::uuid)) "
      "ORDER BY m.created_at, m.id LIMIT $3::int",
      std::move(params));
  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  for (const auto& r : rows) out["items"].append(json::parse(r["j"].as<std::string>()));
  co_return out;
}

Task<Json::Value> contextNow(ToolCall& c) {
  const auto now = co_await agent::loadNow(c.tx, c.principal.userId);
  Json::Value args;
  args["from"] = now.dayFrom;
  args["to"] = now.dayTo;
  const auto cal = co_await tools::registry().runInTx("calendar.view", c.principal, c.tx, args);
  const auto pending = co_await c.tx.exec(
      "SELECT count(*)::int AS n FROM approvals WHERE status = 'pending' AND user_id = $1::uuid", c.principal.userId);
  Json::Value out;
  out["now"] = now.localNow;
  out["local_day"] = now.localDay;
  out["timezone"] = now.timezone;
  out["day_start_hour"] = now.dayStartHour;
  out["today"] = cal["items"];
  out["pending_approvals"] = pending[0]["n"].as<int>();
  out["agent_writes_enabled"] = now.agentWritesEnabled;
  const auto epoch = co_await c.tx.exec("SELECT extract(epoch FROM now())::bigint AS n");
  const auto day = co_await scoring::gatherDay(c.tx, c.principal.userId, now.localDay);
  out["progress"] = scoring::progress(day, epoch[0]["n"].as<int64_t>());
  co_return out;
}

}  // namespace

void bindAgent(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "approvals", .table = "approvals", .filters = {}, .validate = nullptr});
  reg.bind("approvals.approve", [](ToolCall& c) { return decide(c, true); });
  reg.bind("approvals.reject", [](ToolCall& c) { return decide(c, false); });

  crud::bindCrud(reg, {.prefix = "conversations",
                       .table = "conversations",
                       .filters = {{"from", "r.local_day >= $?::date"}, {"to", "r.local_day <= $?::date"}},
                       .validate = nullptr});
  reg.bind("conversations.messages", conversationMessages);

  reg.bind("context.now", contextNow);
  // The agent loop handles this call itself (it changes the tool set of the running turn); the binding keeps
  // the registry complete and answers direct calls.
  reg.bind("agent.load_tools", [](ToolCall& c) -> Task<Json::Value> {
    Json::Value out;
    out["loaded"] = Json::Value(Json::arrayValue);
    const auto& groups = tools::registry().manifest().agentGroups;
    if (const auto it = groups.find(c.args["group"].asString()); it != groups.end()) {
      for (const auto& n : it->second) out["loaded"].append(n);
    }
    co_return out;
  });
}

}  // namespace thelife::modules
