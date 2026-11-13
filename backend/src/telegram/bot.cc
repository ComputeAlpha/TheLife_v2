#include "telegram/bot.h"

#include <drogon/drogon.h>
#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "agent/runner.h"
#include "agent/store.h"
#include "core/blocking.h"
#include "core/crypto.h"
#include "core/env.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/telegram_store.h"
#include "jobs/worker.h"
#include "llm/transport.h"
#include "tools/registry.h"

namespace thelife::telegram {

namespace {

using drogon::Task;

std::string token, apiBase, botKey;
int pollSeconds = 25;
std::atomic<bool> on{false};
std::array<unsigned char, crypto_auth_hmacsha256_KEYBYTES> macKey{};

trantor::ConcurrentTaskQueue& pool() {
  static trantor::ConcurrentTaskQueue p(2, "telegram");
  return p;
}

const std::shared_ptr<llm::Transport>& transport() {
  static const std::shared_ptr<llm::Transport> t(llm::makeCurlTransport());
  return t;
}

// Never logged: the token is part of the URL.
std::string methodUrl(const std::string& method) { return apiBase + "/bot" + token + "/" + method; }

Json::Value callBlocking(const std::string& method, const Json::Value& body, int timeoutSec) {
  const auto r = transport()->post(methodUrl(method), {{"Content-Type", "application/json"}}, json::compact(body),
                                   timeoutSec, timeoutSec, {});
  Json::Value v;
  try {
    v = json::parse(r.body.empty() ? "{}" : r.body);
  } catch (const std::exception&) {
  }
  if (r.status != 200 || !v["ok"].asBool()) {
    LOG_WARN << "telegram " << method << " failed: status " << r.status
             << (v["description"].isString() ? " " + v["description"].asString().substr(0, 120) : "");
    return Json::Value();
  }
  return v["result"];
}

Task<Json::Value> call(std::string method, Json::Value body) {
  co_return co_await runBlockingOn(pool(), [m = std::move(method), b = std::move(body)] { return callBlocking(m, b, 20); });
}

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

Task<void> reply(int64_t chat, std::string text) { co_await send(chat, std::move(text)); }

// A message from a private chat.
Task<void> handleMessage(const Json::Value& msg) {
  const auto& chat = msg["chat"];
  if (chat["type"].asString() != "private") {  // groups and channels: dropped, never answered (no information leak)
    co_await runBlockingOn(pool(), [] { db::telegram::countDropped(botKey, 1); });
    co_return;
  }
  const int64_t chatId = chat["id"].asInt64();
  const auto user = co_await db::telegram::chatUser(chatId);
  const std::string text = msg["text"].isString() ? msg["text"].asString() : "";
  if (!user) {
    if (text.rfind("/link", 0) == 0) {
      std::string code = trim(text.substr(5));
      std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      const auto linked = code.empty() ? std::nullopt
                                       : co_await db::telegram::link(crypto::hex(crypto::sha256(code)), chatId,
                                                                     msg["from"]["username"].asString());
      co_await reply(chatId, linked ? "Linked to TheLife. Tell me what you did or plan to do; /help lists the commands."
                                    : "That code is not valid. Get a new one in TheLife's settings; codes expire after "
                                      "10 minutes.");
    } else if (text == "/start") {
      co_await reply(chatId, "To use this bot, open TheLife's settings, get a link code and send: /link CODE");
    } else {
      co_await runBlockingOn(pool(), [] { db::telegram::countDropped(botKey, 1); });
    }
    co_return;
  }
  if (text.empty()) {
    co_await reply(chatId, "I can only read text messages for now.");
    co_return;
  }
  if (text.rfind("/link", 0) == 0) {
    co_await reply(chatId, "This chat is already linked.");
    co_return;
  }

  auth::Principal p;
  p.kind = auth::PrincipalKind::User;
  p.tenantId = user->tenantId;
  p.userId = user->userId;
  auto tx = co_await db::TenantTx::begin(p);
  const auto conv = co_await agent::ensureConversation(tx, p.userId, "telegram");
  // Telegram may deliver an update again after a restart: its message id makes the send idempotent.
  const std::string clientId = "tg:" + std::to_string(chatId) + ":" + std::to_string(msg["message_id"].asInt64());
  const auto seen = co_await tx.exec(
      "SELECT 1 FROM messages WHERE conversation_id = $1::uuid AND client_msg_id = $2", conv.id, clientId);
  if (!seen.empty()) {
    co_await tx.commit();
    co_return;
  }
  const auto turnId = ids::newUuidV7();
  agent::StoredMessage m;
  m.conversationId = conv.id;
  m.turnId = turnId;
  m.role = "user";
  m.content = text.size() > 8000 ? text.substr(0, 8000) : text;
  m.clientMsgId = clientId;
  const auto messageId = co_await agent::insertMessage(tx, std::move(m));
  co_await tx.exec(
      "INSERT INTO turns (id, tenant_id, user_id, conversation_id, message_id, trigger) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::uuid, 'user_message')",
      turnId, p.tenantId, p.userId, conv.id, messageId);
  co_await tx.commit();
  co_await typing(chatId);
  agent::TurnRequest r;
  r.tenantId = p.tenantId;
  r.userId = p.userId;
  r.channel = "telegram";
  r.trigger = "user_message";
  r.turnId = turnId;
  agent::submitTurn(r);
}

Task<void> answerCallback(std::string callbackId, std::string text) {
  Json::Value b;
  b["callback_query_id"] = std::move(callbackId);
  b["text"] = std::move(text);
  co_await call("answerCallbackQuery", b);
}

// Approve / Reject buttons: "ap:<approval id>:<a|r>:<mac>", the mac binding the button to the user.
Task<void> handleCallback(const Json::Value& cb) {
  const auto data = cb["data"].asString();
  const auto cbId = cb["id"].asString();
  const auto user = co_await db::telegram::chatUser(cb["from"]["id"].asInt64());
  if (!user || data.size() < 44 || data.rfind("ap:", 0) != 0 || data[39] != ':' || data[41] != ':') {
    co_await answerCallback(cbId, "This button is not valid.");
    co_return;
  }
  const auto approvalId = data.substr(3, 36);
  const char action = data[40];
  const auto mac = data.substr(42);
  const auto expected = callbackMac(user->userId, approvalId, action);
  if ((action != 'a' && action != 'r') || !ids::isUuid(approvalId) || mac.size() != expected.size() ||
      sodium_memcmp(mac.data(), expected.data(), mac.size()) != 0) {
    LOG_WARN << "telegram: callback with an invalid signature dropped";
    co_await answerCallback(cbId, "This button is not valid.");
    co_return;
  }
  auth::Principal p;
  p.kind = auth::PrincipalKind::User;
  p.tenantId = user->tenantId;
  p.userId = user->userId;
  Json::Value args;
  args["id"] = approvalId;
  std::string outcome;
  std::optional<ApiError> err;
  try {
    const auto r = co_await tools::registry().invoke(action == 'a' ? "approvals.approve" : "approvals.reject", p, args,
                                                     tools::InvokeOptions{});
    const auto status = r.value["status"].asString();
    outcome = status == "approved" ? "Approved and done." : status == "rejected" ? "Rejected; nothing changed."
              : status == "stale" ? "The item changed since, so nothing ran." : "Approval is " + status + ".";
  } catch (const ApiError& e) {
    err = e;
  }
  if (err) outcome = err->detail().empty() ? err->title() : err->detail();
  co_await answerCallback(cbId, outcome);
  Json::Value edit;
  edit["chat_id"] = cb["message"]["chat"]["id"];
  edit["message_id"] = cb["message"]["message_id"];
  edit["text"] = cb["message"]["text"].asString() + "\n\n→ " + outcome;
  co_await call("editMessageText", edit);
}

void pollLoop() {
  int64_t offset = 0;
  try {
    offset = db::telegram::loadOffset(botKey);
  } catch (const std::exception& e) {
    LOG_ERROR << "telegram: cannot read the poll position: " << e.what();
  }
  for (;;) {
    Json::Value body;
    body["offset"] = static_cast<Json::Int64>(offset);
    body["timeout"] = pollSeconds;
    body["allowed_updates"].append("message");
    body["allowed_updates"].append("callback_query");
    const auto r = transport()->post(methodUrl("getUpdates"), {{"Content-Type", "application/json"}}, json::compact(body),
                                     pollSeconds + 15, pollSeconds + 15, {});
    Json::Value v;
    try {
      v = json::parse(r.body.empty() ? "{}" : r.body);
    } catch (const std::exception&) {
    }
    if (r.status != 200 || !v["ok"].asBool()) {
      if (r.status == 401 || r.status == 404) LOG_ERROR << "telegram: the bot token was rejected; retrying in 60 s";
      std::this_thread::sleep_for(std::chrono::seconds(r.status == 401 || r.status == 404 ? 60 : 5));
      continue;
    }
    std::vector<Json::Value> updates;
    for (const auto& u : v["result"]) {
      offset = std::max(offset, u["update_id"].asInt64() + 1);
      updates.push_back(u);
    }
    if (updates.empty()) continue;
    try {
      db::telegram::saveOffset(botKey, offset);  // acknowledged on the next poll
    } catch (const std::exception& e) {
      LOG_ERROR << "telegram: poll position not saved: " << e.what();
    }
    // In order, one after another (a chat's messages keep their order).
    drogon::app().getLoop()->queueInLoop([updates] {
      drogon::async_run([updates]() -> Task<void> {
        for (const auto& u : updates) {
          try {
            if (u.isMember("message")) co_await handleMessage(u["message"]);
            else if (u.isMember("callback_query")) co_await handleCallback(u["callback_query"]);
          } catch (const std::exception& e) {
            LOG_ERROR << "telegram: update " << u["update_id"].asInt64() << " failed: " << e.what();
          }
        }
      });
    });
  }
}

// A notification routed to Telegram (agent_lld.md §8 rule 3). Skipped when it was acked meanwhile. An agent message
// is sent in full and copied into the Telegram conversation, so a reply there has it in context.
Task<void> deliver(const jobs::Job& job) {
  const auto id = job.payload["notification_id"].asString();
  std::optional<int64_t> chat;
  std::string text, userId, messageId;
  {
    auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
    const auto n = co_await tx.exec(
        "SELECT n.user_id::text AS u, n.title, n.body, n.resolved_at IS NOT NULL OR coalesce(n.expires_at < now(), false) AS done, "
        "  coalesce(n.message_id::text, '') AS msg, coalesce(m.content, '') AS full_text, "
        "  coalesce(c.channel, '') AS channel "
        "FROM notifications n LEFT JOIN messages m ON m.id = n.message_id "
        "LEFT JOIN conversations c ON c.id = m.conversation_id WHERE n.id = $1::uuid",
        id);
    if (n.empty()) co_return;
    userId = n[0]["u"].as<std::string>();
    chat = enabled() ? co_await chatOf(tx, userId) : std::nullopt;
    if (n[0]["done"].as<bool>() || !chat) {
      co_await tx.exec("UPDATE notification_deliveries SET status = 'skipped', at = now() "
                       "WHERE notification_id = $1::uuid AND channel = 'telegram' AND status = 'queued'", id);
      co_await tx.commit();
      co_return;
    }
    const auto fullText = n[0]["full_text"].as<std::string>();
    if (!fullText.empty()) {
      text = fullText;
      if (n[0]["channel"].as<std::string>() != "telegram") messageId = n[0]["msg"].as<std::string>();
    } else {
      text = n[0]["title"].as<std::string>() + "\n" + n[0]["body"].as<std::string>();
    }
    co_await tx.commit();
  }
  const auto sent = co_await send(*chat, text);
  if (!sent) throw std::runtime_error("telegram send failed");  // retried with backoff
  auto tx = co_await db::TenantTx::beginSystem(job.tenantId);
  co_await tx.exec("UPDATE notification_deliveries SET status = 'sent', at = now() "
                   "WHERE notification_id = $1::uuid AND channel = 'telegram'", id);
  if (!messageId.empty()) {
    const auto conv = co_await agent::ensureConversation(tx, userId, "telegram");
    agent::StoredMessage m;
    m.conversationId = conv.id;
    m.role = "assistant";
    m.content = text;
    co_await agent::insertMessage(tx, std::move(m));
  }
  co_await tx.commit();
}

}  // namespace

void registerJobs() { jobs::registerHandler("notify.telegram", deliver); }

void init() {
  token = env::get("THELIFE_TELEGRAM_BOT_TOKEN");
  apiBase = env::get("THELIFE_TELEGRAM_API", "https://api.telegram.org");
  while (!apiBase.empty() && apiBase.back() == '/') apiBase.pop_back();
  pollSeconds = env::getInt("THELIFE_TELEGRAM_POLL_S", 25, 0, 50);
  if (token.empty()) {
    LOG_INFO << "telegram: off (THELIFE_TELEGRAM_BOT_TOKEN unset)";
    return;
  }
  const auto key = crypto::sha256(token + "|thelife-callback-buttons");
  std::copy_n(key.begin(), macKey.size(), macKey.begin());
  botKey = crypto::hex(crypto::sha256(apiBase + "|" + token)).substr(0, 16);
  on = true;
  std::thread(pollLoop).detach();
  LOG_INFO << "telegram: polling for updates";
}

bool enabled() { return on.load(); }

std::string callbackMac(const std::string& userId, const std::string& approvalId, char action) {
  const std::string msg = userId + ":" + approvalId + ":" + action;
  std::array<unsigned char, crypto_auth_hmacsha256_BYTES> out{};
  crypto_auth_hmacsha256(out.data(), reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), macKey.data());
  // 16 base64url characters (96 bits): "ap:" + 36 + ":a:" + 16 = 58 bytes, inside Telegram's 64-byte limit.
  char b64[sodium_base64_ENCODED_LEN(12, sodium_base64_VARIANT_URLSAFE_NO_PADDING)];
  sodium_bin2base64(b64, sizeof b64, out.data(), 12, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  return std::string(b64);
}

drogon::Task<std::optional<int64_t>> chatOf(db::TenantTx& tx, const std::string& userId) {
  if (!enabled()) co_return std::nullopt;
  const auto r = co_await tx.exec("SELECT chat_id FROM telegram_links WHERE user_id = $1::uuid AND revoked_at IS NULL",
                                  userId);
  if (r.empty()) co_return std::nullopt;
  co_return r[0]["chat_id"].as<int64_t>();
}

drogon::Task<std::optional<int64_t>> send(int64_t chatId, std::string text, Json::Value markup) {
  if (!enabled()) co_return std::nullopt;
  std::optional<int64_t> last;
  // Telegram allows 4096 characters per message; split on line breaks where possible.
  while (!text.empty()) {
    std::string part = text;
    if (part.size() > 4000) {
      auto cut = text.rfind('\n', 4000);
      if (cut == std::string::npos || cut < 2000) {
        cut = 4000;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;  // not inside a character
      } else {
        ++cut;  // keep the line break with its line
      }
      part = text.substr(0, cut);
    }
    text.erase(0, part.size());
    Json::Value b;
    b["chat_id"] = static_cast<Json::Int64>(chatId);
    b["text"] = part;
    b["link_preview_options"]["is_disabled"] = true;
    if (text.empty() && !markup.isNull()) b["reply_markup"] = markup;
    const auto r = co_await call("sendMessage", b);
    if (r.isNull()) co_return std::nullopt;
    last = r["message_id"].asInt64();
  }
  co_return last;
}

drogon::Task<void> typing(int64_t chatId) {
  if (!enabled()) co_return;
  Json::Value b;
  b["chat_id"] = static_cast<Json::Int64>(chatId);
  b["action"] = "typing";
  co_await call("sendChatAction", b);
}

drogon::Task<void> sendApproval(int64_t chatId, const std::string& userId, const std::string& approvalId,
                                const std::string& summary) {
  Json::Value markup;
  Json::Value row(Json::arrayValue);
  for (const char a : {'a', 'r'}) {
    Json::Value btn;
    btn["text"] = a == 'a' ? "Approve" : "Reject";
    btn["callback_data"] = "ap:" + approvalId + ":" + a + ":" + callbackMac(userId, approvalId, a);
    row.append(btn);
  }
  markup["inline_keyboard"].append(row);
  std::string text = "Needs your approval: " + (summary.size() > 900 ? summary.substr(0, 900) + "…" : summary);
  co_await send(chatId, text, markup);
}

}  // namespace thelife::telegram
