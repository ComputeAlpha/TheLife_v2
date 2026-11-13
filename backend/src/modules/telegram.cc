// Telegram linking (api/resources/telegram.yaml, agent_lld.md §7.2). The bot itself lives in telegram/bot.cc.
#include <sodium.h>

#include "core/crypto.h"
#include "modules/modules.h"
#include "telegram/bot.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

Task<Json::Value> status(ToolCall& c) {
  const auto r = co_await c.tx.exec(
      "SELECT username, to_char(linked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS at "
      "FROM telegram_links WHERE user_id = $1::uuid AND revoked_at IS NULL",
      c.principal.userId);
  Json::Value out;
  out["enabled"] = telegram::enabled();
  out["linked"] = !r.empty();
  out["username"] = r.empty() || r[0]["username"].isNull() ? Json::Value() : Json::Value(r[0]["username"].as<std::string>());
  out["linked_at"] = r.empty() ? Json::Value() : Json::Value(r[0]["at"].as<std::string>());
  co_return out;
}

Task<Json::Value> createCode(ToolCall& c) {
  // 8 characters from an alphabet without look-alikes (about 40 bits; single use, 10 minutes, hashed at rest).
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::string code;
  for (int i = 0; i < 8; ++i) code.push_back(kAlphabet[randombytes_uniform(sizeof kAlphabet - 1)]);
  // Earlier unused codes of this user stop working.
  co_await c.tx.exec("UPDATE telegram_link_codes SET used_at = now() WHERE user_id = $1::uuid AND used_at IS NULL",
                     c.principal.userId);
  const auto r = co_await c.tx.exec(
      "INSERT INTO telegram_link_codes (code_hash, tenant_id, user_id) VALUES ($1, $2::uuid, $3::uuid) "
      "RETURNING to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS exp",
      crypto::hex(crypto::sha256(code)), c.principal.tenantId, c.principal.userId);
  Json::Value out;
  out["code"] = code;
  out["expires_at"] = r[0]["exp"].as<std::string>();
  co_return out;
}

Task<Json::Value> unlink(ToolCall& c) {
  co_await c.tx.exec("UPDATE telegram_links SET revoked_at = now() WHERE user_id = $1::uuid AND revoked_at IS NULL",
                     c.principal.userId);
  co_return Json::Value();
}

}  // namespace

void bindTelegram(tools::ToolRegistry& reg) {
  reg.bind("telegram.link_status", status);
  reg.bind("telegram.create_link_code", createCode);
  reg.bind("telegram.unlink", unlink);
}

}  // namespace thelife::modules
