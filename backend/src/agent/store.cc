#include "agent/store.h"

#include <algorithm>
#include <cstdlib>

#include "core/ids.h"
#include "core/json.h"
#include "tools/manifest.h"

namespace thelife::agent {

drogon::Task<NowInfo> loadNow(db::TenantTx& tx, const std::string& userId) {
  const auto r = co_await tx.exec(
      "WITH u AS (SELECT display_name, timezone, day_start_hour, (now() AT TIME ZONE timezone) AS lt "
      "           FROM users WHERE id = $1::uuid), "
      "d AS (SELECT u.*, (lt - make_interval(hours => day_start_hour))::date AS day FROM u) "
      "SELECT display_name, timezone, day_start_hour, to_char(lt, 'YYYY-MM-DD\"T\"HH24:MI:SS') AS lt, "
      "  round(extract(epoch FROM (lt - (now() AT TIME ZONE 'UTC'))) / 60)::int AS off_min, "
      "  to_char(day, 'YYYY-MM-DD') AS day, to_char(day, 'FMDay') AS weekday, "
      "  to_char((day + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "          'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS dfrom, "
      "  to_char(((day + 1) + make_interval(hours => day_start_hour)) AT TIME ZONE timezone, "
      "          'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS dto, "
      "  (SELECT agent_writes_enabled FROM tenants) AS writes "
      "FROM d",
      userId);
  if (r.empty()) throw std::runtime_error("user not found");
  const auto& row = r[0];
  NowInfo n;
  n.userName = row["display_name"].as<std::string>();
  n.timezone = row["timezone"].as<std::string>();
  n.dayStartHour = row["day_start_hour"].as<int>();
  const int off = row["off_min"].as<int>();
  const int a = std::min(std::abs(off), 23 * 60 + 59);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%c%02d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
  n.offset = buf;
  n.offsetMinutes = off;
  n.localNow = row["lt"].as<std::string>() + n.offset;
  n.localDay = row["day"].as<std::string>();
  n.weekday = row["weekday"].as<std::string>();
  n.dayFrom = row["dfrom"].as<std::string>();
  n.dayTo = row["dto"].as<std::string>();
  n.agentWritesEnabled = row["writes"].isNull() || row["writes"].as<bool>();
  co_return n;
}

drogon::Task<ConversationRef> ensureConversation(db::TenantTx& tx, const std::string& userId,
                                                 const std::string& channel) {
  const auto id = ids::newUuidV7();
  co_await tx.exec(
      "INSERT INTO conversations (id, tenant_id, created_by_kind, created_by_id, user_id, channel, local_day) "
      "SELECT $1::uuid, u.tenant_id, 'user', u.id, u.id, $3, "
      "       ((now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date "
      "FROM users u WHERE u.id = $2::uuid "
      "ON CONFLICT (tenant_id, user_id, channel, local_day) DO NOTHING",
      id, userId, channel);
  const auto r = co_await tx.exec(
      "SELECT c.id::text AS id, to_char(c.local_day, 'YYYY-MM-DD') AS day FROM conversations c JOIN users u "
      "  ON u.id = c.user_id "
      "WHERE c.user_id = $1::uuid AND c.channel = $2 "
      "  AND c.local_day = ((now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date",
      userId, channel);
  if (r.empty()) throw std::runtime_error("conversation not created");
  ConversationRef ref{r[0]["id"].as<std::string>(), r[0]["day"].as<std::string>()};
  // Day close (§6.2): earlier conversations of this channel are closed once a new day's one exists.
  co_await tx.exec(
      "UPDATE conversations SET status = 'closed', closed_at = now() "
      "WHERE user_id = $1::uuid AND channel = $2 AND status = 'open' AND local_day < $3::date",
      userId, channel, ref.localDay);
  co_return ref;
}

Json::Value toolCallsJson(const std::vector<llm::ToolCallOut>& calls) {
  Json::Value arr(Json::arrayValue);
  for (const auto& c : calls) {
    Json::Value j;
    j["id"] = c.id;
    j["name"] = c.name;
    j["arguments"] = c.arguments;
    arr.append(j);
  }
  return arr;
}

std::vector<llm::ToolCallOut> toolCallsFrom(const Json::Value& v) {
  std::vector<llm::ToolCallOut> out;
  for (const auto& c : v) out.push_back({c["id"].asString(), c["name"].asString(), c["arguments"].asString()});
  return out;
}

namespace {
std::optional<std::string> opt(const std::string& s) { return s.empty() ? std::nullopt : std::optional(s); }
}  // namespace

drogon::Task<std::string> insertMessage(db::TenantTx& tx, StoredMessage m) {
  if (m.id.empty()) m.id = ids::newUuidV7();
  std::vector<std::optional<std::string>> params{
      m.id, tx.tenantId(), m.conversationId, opt(m.turnId), m.role, m.kind, m.content,
      json::compact(toolCallsJson(m.toolCalls)), opt(m.toolCallId), opt(m.toolName), opt(m.clientMsgId),
      opt(m.promptId), opt(m.promptVersion), opt(m.contractVersion), db::nullable(m.tokensIn),
      db::nullable(m.tokensOut)};
  co_await tx.execParams(
      "INSERT INTO messages (id, tenant_id, conversation_id, turn_id, role, kind, content, tool_calls, tool_call_id, "
      "tool_name, client_msg_id, prompt_id, prompt_version, contract_version, tokens_in, tokens_out) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5, $6, $7, $8::jsonb, $9, $10, $11, $12, $13, $14, "
      "$15::int, $16::int)",
      std::move(params));
  co_return m.id;
}

drogon::Task<std::vector<StoredMessage>> loadWorkingMemory(db::TenantTx& tx, const std::string& userId,
                                                           const std::string& channel, const std::string& localDay) {
  const auto r = co_await tx.exec(
      "SELECT m.id::text AS id, m.conversation_id::text AS conv, coalesce(m.turn_id::text, '') AS turn, m.role, "
      "  m.kind, m.content, m.tool_calls::text AS calls, coalesce(m.tool_call_id, '') AS tcid, "
      "  coalesce(m.tool_name, '') AS tname "
      "FROM messages m JOIN conversations c ON c.id = m.conversation_id "
      "WHERE c.user_id = $1::uuid AND c.channel = $2 AND c.local_day >= $3::date - 1 AND c.local_day <= $3::date "
      "  AND m.kind <> 'rolling_summary' "
      "ORDER BY c.local_day, m.created_at, m.id",
      userId, channel, localDay);
  std::vector<StoredMessage> out;
  for (const auto& row : r) {
    StoredMessage m;
    m.id = row["id"].as<std::string>();
    m.conversationId = row["conv"].as<std::string>();
    m.turnId = row["turn"].as<std::string>();
    m.role = row["role"].as<std::string>();
    m.kind = row["kind"].as<std::string>();
    m.content = row["content"].as<std::string>();
    m.toolCalls = toolCallsFrom(json::parse(row["calls"].as<std::string>()));
    m.toolCallId = row["tcid"].as<std::string>();
    m.toolName = row["tname"].as<std::string>();
    out.push_back(std::move(m));
  }
  co_return out;
}

drogon::Task<void> insertAudit(db::TenantTx& tx, const AuditEntry& a) {
  auto cut = [](const std::optional<std::string>& s) -> std::optional<std::string> {
    if (!s) return s;
    return s->size() > 1000 ? s->substr(0, 997) + "..." : *s;
  };
  std::vector<std::optional<std::string>> params{
      ids::newUuidV7(), tx.tenantId(), a.userId, opt(a.conversationId), opt(a.turnId), opt(a.messageId), a.tool,
      json::compact(a.args.isNull() ? Json::Value(Json::objectValue) : a.args), a.decision, a.outcome, a.approvalId,
      cut(a.resultSummary), cut(a.error), tools::embeddedManifest().contractVersion};
  co_await tx.execParams(
      "INSERT INTO agent_actions (id, tenant_id, user_id, conversation_id, turn_id, message_id, tool, args, decision, "
      "outcome, approval_id, result_summary, error, contract_version) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::uuid, $6::uuid, $7, $8::jsonb, $9, $10, $11::uuid, $12, "
      "$13, $14)",
      std::move(params));
}

}  // namespace thelife::agent
