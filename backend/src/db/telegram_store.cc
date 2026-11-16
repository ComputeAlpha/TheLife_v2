#include "db/telegram_store.h"

#include "db/database.h"

namespace thelife::db::telegram {

drogon::Task<std::optional<ChatUser>> chatUser(int64_t chatId) {
  const auto r = co_await Database::instance().client()->execSqlCoro(
      "SELECT tenant_id::text AS t, user_id::text AS u FROM ops.telegram_chat_user($1::bigint)", std::to_string(chatId));
  if (r.empty()) co_return std::nullopt;
  co_return ChatUser{r[0]["t"].as<std::string>(), r[0]["u"].as<std::string>()};
}

drogon::Task<std::optional<ChatUser>> link(std::string codeHash, int64_t chatId, std::string username) {
  const auto r = co_await Database::instance().client()->execSqlCoro(
      "SELECT tenant_id::text AS t, user_id::text AS u FROM ops.telegram_link($1, $2::bigint, $3)", codeHash,
      std::to_string(chatId), username);
  if (r.empty()) co_return std::nullopt;
  co_return ChatUser{r[0]["t"].as<std::string>(), r[0]["u"].as<std::string>()};
}

void countDropped(const std::string& bot, int n) {
  Database::instance().client()->execSqlSync(
      "INSERT INTO ops.telegram_state (bot, dropped) VALUES ($1, $2::int) ON CONFLICT (bot) DO UPDATE "
      "SET dropped = ops.telegram_state.dropped + EXCLUDED.dropped, updated_at = now()",
      bot, std::to_string(n));
}

}  // namespace thelife::db::telegram
