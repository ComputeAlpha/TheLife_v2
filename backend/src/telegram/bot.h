#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "db/tenant_tx.h"

// Telegram channel (agent_lld.md §7.2): long polling (no public webhook), linking by one-time code, private chats
// of linked users only, turns on the telegram channel, plain-text replies, approvals with HMAC-signed buttons.
//   THELIFE_TELEGRAM_BOT_TOKEN   bot token (unset: Telegram is off)
//   THELIFE_TELEGRAM_API         API base (default https://api.telegram.org; tests use a fake)
//   THELIFE_TELEGRAM_POLL_S      long-poll timeout (default 25)
namespace thelife::telegram {

void init();  // reads the environment and starts the poll thread when a token is set
bool enabled();
void registerJobs();  // notify.telegram (notification fallback, agent_lld.md §8)

// The user's linked chat, if any (in the caller's tenant transaction).
drogon::Task<std::optional<int64_t>> chatOf(db::TenantTx& tx, const std::string& userId);

// Sends plain text (split at Telegram's limit); markup is an optional reply_markup. Returns the last message id,
// or nullopt on failure (logged; never throws).
drogon::Task<std::optional<int64_t>> send(int64_t chatId, std::string text, Json::Value markup = Json::Value());
drogon::Task<void> typing(int64_t chatId);

// An approval as a message with Approve / Reject buttons; callback_data is "ap:<id>:<a|r>:<mac>" (≤ 64 bytes).
drogon::Task<void> sendApproval(int64_t chatId, const std::string& userId, const std::string& approvalId,
                                const std::string& summary);
std::string callbackMac(const std::string& userId, const std::string& approvalId, char action);

}  // namespace thelife::telegram
