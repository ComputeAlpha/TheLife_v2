#include "http/chat_routes.h"

#include <drogon/drogon.h>

#include <algorithm>

#include "agent/events.h"
#include "agent/runner.h"
#include "agent/store.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/tenant_tx.h"
#include "http/route.h"

namespace thelife::http {

namespace {

bool visibleAscii(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c > ' ' && c < 127; });
}

drogon::Task<Json::Value> turnJson(db::TenantTx& tx, const std::string& turnId) {
  const auto r = co_await tx.exec(
      "SELECT jsonb_build_object('id', id, 'conversation_id', conversation_id, 'message_id', message_id, "
      "  'status', status, 'error', error, 'created_at', created_at, 'finished_at', finished_at)::text AS j "
      "FROM turns WHERE id = $1::uuid AND message_id IS NOT NULL",
      turnId);
  if (r.empty()) throw ApiError::notFound();
  co_return json::parse(r[0]["j"].as<std::string>());
}

drogon::Task<drogon::HttpResponsePtr> send(RequestContext& ctx) {
  const auto body = ctx.jsonBody();
  BodyReader br{body, {"text", "client_msg_id"}};
  br.rejectUnknown();
  const auto text = *br.string("text", 1, 32000, true);  // bytes here; characters below
  const auto chars = std::count_if(text.begin(), text.end(), [](char c) { return (c & 0xC0) != 0x80; });
  if (chars < 1 || chars > 8000) throw ApiError::validation({{"/text", "length must be between 1 and 8000"}});
  if (text.find('\0') != std::string::npos) throw ApiError::validation({{"/text", "must not contain NUL"}});
  auto clientId = br.string("client_msg_id", 8, 128, false);
  if (clientId && !visibleAscii(*clientId)) {
    throw ApiError::validation({{"/client_msg_id", "must be visible ASCII"}});
  }
  // Idempotency-Key works like client_msg_id (one of them is enough).
  const auto& headers = ctx.req->headers();
  if (const auto h = headers.find("idempotency-key"); h != headers.end()) {
    if (h->second.size() < 8 || h->second.size() > 128 || !visibleAscii(h->second)) {
      throw ApiError::validation({{"Idempotency-Key", "must be 8 to 128 visible ASCII characters"}});
    }
    if (!clientId) clientId = h->second;
  }

  const auto& who = ctx.who();
  auto tx = co_await db::TenantTx::begin(who);
  const auto conv = co_await agent::ensureConversation(tx, who.userId, "web");
  if (clientId) {
    const auto prev = co_await tx.exec(
        "SELECT t.id::text AS id FROM turns t JOIN messages m ON m.id = t.message_id "
        "WHERE m.conversation_id = $1::uuid AND m.client_msg_id = $2",
        conv.id, *clientId);
    if (!prev.empty()) {
      auto existing = co_await turnJson(tx, prev[0]["id"].as<std::string>());
      co_await tx.commit();
      co_return jsonResponse(existing, drogon::k202Accepted);
    }
  }
  const auto turnId = ids::newUuidV7();
  agent::StoredMessage m;
  m.conversationId = conv.id;
  m.turnId = turnId;
  m.role = "user";
  m.content = text;
  if (clientId) m.clientMsgId = *clientId;
  const auto messageId = co_await agent::insertMessage(tx, std::move(m));
  co_await tx.exec(
      "INSERT INTO turns (id, tenant_id, user_id, conversation_id, message_id, trigger) "
      "VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid, $5::uuid, 'user_message')",
      turnId, who.tenantId, who.userId, conv.id, messageId);
  auto turn = co_await turnJson(tx, turnId);
  co_await tx.commit();

  agent::TurnRequest r;
  r.tenantId = who.tenantId;
  r.userId = who.userId;
  r.channel = "web";
  r.trigger = "user_message";
  r.turnId = turnId;
  agent::submitTurn(std::move(r));
  co_return jsonResponse(turn, drogon::k202Accepted);
}

drogon::Task<drogon::HttpResponsePtr> turn(RequestContext& ctx) {
  const auto& params = ctx.req->getRoutingParameters();
  const auto id = params.empty() ? std::string() : params[0];
  if (!ids::isUuid(id)) throw ApiError::validation({{"id", "must be a UUID"}});
  auto tx = co_await db::TenantTx::begin(ctx.who());
  auto out = co_await turnJson(tx, id);
  co_await tx.commit();
  co_return jsonResponse(out);
}

drogon::Task<drogon::HttpResponsePtr> stream(RequestContext& ctx) {
  const auto userId = ctx.who().userId;
  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [userId](drogon::ResponseStreamPtr s) { agent::EventHub::instance().subscribe(userId, std::move(s)); },
      /*disableKickoffTimeout=*/true);
  resp->setContentTypeString("text/event-stream");
  resp->addHeader("Cache-Control", "no-store");
  resp->addHeader("X-Accel-Buffering", "no");
  co_return resp;
}

}  // namespace

void registerChatRoutes() {
  addRoute("/api/v1/chat/web/messages", drogon::Post, Access::Authenticated, send);
  addRoute("/api/v1/chat/turns/{id}", drogon::Get, Access::Authenticated, turn);
  addRoute("/api/v1/chat/web/stream", drogon::Get, Access::Authenticated, stream);
  drogon::app().getLoop()->runEvery(20.0, [] { agent::EventHub::instance().heartbeat(); });
}

}  // namespace thelife::http
