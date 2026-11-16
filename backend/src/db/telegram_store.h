#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>

// Pre-tenant paths of the Telegram bot (an incoming chat id belongs to no tenant until it is looked up) and its
// dropped-update counter. Everything else about Telegram runs in a TenantTx.
namespace thelife::db::telegram {

struct ChatUser {
  std::string tenantId, userId;
};

drogon::Task<std::optional<ChatUser>> chatUser(int64_t chatId);
// Consumes a link code (sha256 hex) and links the chat; nullopt when the code is unknown, used or expired.
drogon::Task<std::optional<ChatUser>> link(std::string codeHash, int64_t chatId, std::string username);

// Updates dropped from unlinked or group chats, counted per bot (`bot`: a hash of the API base and token, never
// the token itself). Blocking. The poll position is not stored: Telegram keeps it (telegram/bot.cc pollLoop).
void countDropped(const std::string& bot, int n);

}  // namespace thelife::db::telegram
