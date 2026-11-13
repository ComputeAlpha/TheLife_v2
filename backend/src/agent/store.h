#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

#include "db/tenant_tx.h"
#include "llm/types.h"

namespace thelife::agent {

// The user's clock right now (timezone, day_start_hour honoured).
struct NowInfo {
  std::string userName, timezone;
  int dayStartHour = 4;
  std::string localNow;   // 2026-11-13T15:06:40+05:30
  std::string offset;     // +05:30
  int offsetMinutes = 0;
  std::string localDay;   // 2026-11-13 (the day that started at day_start_hour)
  std::string weekday;    // Friday
  std::string dayFrom, dayTo;  // the local day as UTC instants
  bool agentWritesEnabled = true;
};
drogon::Task<NowInfo> loadNow(db::TenantTx& tx, const std::string& userId);

struct ConversationRef {
  std::string id;
  std::string localDay;
};
// Today's conversation for the user and channel (created on first use; older open ones are closed).
drogon::Task<ConversationRef> ensureConversation(db::TenantTx& tx, const std::string& userId, const std::string& channel);

struct StoredMessage {
  std::string id, conversationId, turnId, role, kind = "normal", content;
  std::vector<llm::ToolCallOut> toolCalls;
  std::string toolCallId, toolName, clientMsgId, promptId, promptVersion, contractVersion;
  std::optional<int> tokensIn, tokensOut;
};
drogon::Task<std::string> insertMessage(db::TenantTx& tx, StoredMessage m);  // returns the id

// Yesterday's and today's messages of the channel, oldest first (working memory, agent_lld.md §6.2).
drogon::Task<std::vector<StoredMessage>> loadWorkingMemory(db::TenantTx& tx, const std::string& userId,
                                                           const std::string& channel, const std::string& localDay);

struct AuditEntry {
  std::string userId, conversationId, turnId, messageId, tool;
  Json::Value args;
  std::string decision, outcome;
  std::optional<std::string> approvalId, resultSummary, error;
};
drogon::Task<void> insertAudit(db::TenantTx& tx, const AuditEntry& a);

Json::Value toolCallsJson(const std::vector<llm::ToolCallOut>& calls);
std::vector<llm::ToolCallOut> toolCallsFrom(const Json::Value& v);

}  // namespace thelife::agent
