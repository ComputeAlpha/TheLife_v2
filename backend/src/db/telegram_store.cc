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

int64_t loadOffset(const std::string& bot) {
  const auto& c = Database::instance().client();
  c->execSqlSync("INSERT INTO ops.telegram_state (bot) VALUES ($1) ON CONFLICT DO NOTHING", bot);
  return c->execSqlSync("SELECT update_offset FROM ops.telegram_state WHERE bot = $1", bot)[0][0].as<int64_t>();
}

void saveOffset(const std::string& bot, int64_t offset) {
  Database::instance().client()->execSqlSync(
      "UPDATE ops.telegram_state SET update_offset = $2::bigint, updated_at = now() WHERE bot = $1", bot,
      std::to_string(offset));
}

void countDropped(const std::string& bot, int n) {
  Database::instance().client()->execSqlSync(
      "UPDATE ops.telegram_state SET dropped = dropped + $2::int WHERE bot = $1", bot, std::to_string(n));
}

}  // namespace thelife::db::telegram
