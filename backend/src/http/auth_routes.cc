#include "http/auth_routes.h"

#include <set>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <chrono>

#include "auth/rate_limiter.h"
#include "core/blocking.h"
#include "core/crypto.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/database.h"
#include "db/tenant_tx.h"
#include "http/route.h"

namespace thelife::http {

namespace {

using auth::Principal;
using auth::PrincipalKind;
using db::TenantTx;
using drogon::Task;

auth::FailureLimiter& emailLimiter() {
  static auth::FailureLimiter l(5, std::chrono::minutes(15));
  return l;
}
auth::FailureLimiter& ipLimiter() {
  static auth::FailureLimiter l(30, std::chrono::minutes(15));
  return l;
}

constexpr const char* kUserJsonSql =
    "SELECT jsonb_build_object('id', id, 'email', email, 'display_name', display_name, 'timezone', timezone, "
    "'day_start_hour', day_start_hour, 'role', role, 'workdays', to_jsonb(workdays))::text AS j "
    "FROM users WHERE id = $1";

Task<Json::Value> loadUser(TenantTx& tx, const std::string& userId) {
  auto rows = co_await tx.exec(kUserJsonSql, userId);
  if (rows.empty()) throw ApiError::notFound();
  co_return json::parse(rows[0]["j"].as<std::string>());
}

std::string lowerAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Task<drogon::HttpResponsePtr> login(RequestContext& ctx) {
  const auto body = ctx.jsonBody();
  BodyReader in{body, {"email", "password"}};
  in.rejectUnknown();
  const auto email = lowerAscii(*in.string("email", 3, 320, true));
  const auto password = *in.string("password", 1, 1024, true);
  const auto ip = ctx.clientIp();

  if (const int wait = std::max(emailLimiter().blockedFor(email), ipLimiter().blockedFor(ip)); wait > 0) {
    throw ApiError::tooManyRequests(wait);
  }

  const auto rows = co_await db::Database::instance().client()->execSqlCoro(
      "SELECT tenant_id, user_id, password_hash FROM auth.find_login($1)", email);
  bool ok = false;
  if (rows.empty()) {
    co_await runBlocking([password] { crypto::dummyVerify(password); });
  } else {
    const auto hash = rows[0]["password_hash"].as<std::string>();
    ok = co_await runBlocking([password, hash] { return crypto::verifyPassword(password, hash); });
  }
  if (!ok) {
    emailLimiter().recordFailure(email);
    ipLimiter().recordFailure(ip);
    throw ApiError::unauthorized("Invalid email or password.");
  }
  emailLimiter().reset(email);

  Principal p;
  p.kind = PrincipalKind::User;
  p.tenantId = rows[0]["tenant_id"].as<std::string>();
  p.userId = rows[0]["user_id"].as<std::string>();

  const auto token = crypto::newToken(crypto::TokenKind::UserSession);
  auto tx = co_await TenantTx::begin(p);
  const auto created = co_await tx.exec(
      "INSERT INTO credentials (id, tenant_id, kind, user_id, name, token_prefix, token_hash, expires_at) "
      "VALUES ($1, $2, 'user_session', $3, $4, $5, $6::bytea, now() + interval '30 days') "
      "RETURNING to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"') AS expires_at",
      ids::newUuidV7(), p.tenantId, p.userId, std::string("web session"), token.prefix,
      crypto::pgByteaHex(token.hash));
  Json::Value out;
  out["token"] = token.token;
  out["expires_at"] = created[0]["expires_at"].as<std::string>();
  out["user"] = co_await loadUser(tx, p.userId);
  co_await tx.commit();
  co_return jsonResponse(out);
}

Task<drogon::HttpResponsePtr> logout(RequestContext& ctx) {
  auto tx = co_await TenantTx::begin(ctx.who());
  co_await tx.exec("UPDATE credentials SET revoked_at = now() WHERE id = $1 AND revoked_at IS NULL",
                   ctx.who().credentialId);
  co_await tx.commit();
  co_return noContent();
}

Task<drogon::HttpResponsePtr> me(RequestContext& ctx) {
  auto tx = co_await TenantTx::begin(ctx.who());
  auto user = co_await loadUser(tx, ctx.who().userId);
  co_await tx.commit();
  co_return jsonResponse(user);
}

Task<drogon::HttpResponsePtr> updateMe(RequestContext& ctx) {
  const auto body = ctx.jsonBody({"application/merge-patch+json", "application/json"});
  BodyReader in{body, {"display_name", "timezone", "day_start_hour", "workdays"}};
  in.rejectUnknown();
  if (body.empty()) throw ApiError::validation({{"", "at least one property is required"}});
  const auto displayName = in.string("display_name", 1, 200, false);
  const auto timezone = in.string("timezone", 1, 64, false);
  const auto dayStart = in.integer("day_start_hour", 0, 12, false);
  std::optional<std::string> workdays;
  if (body.isMember("workdays")) {
    const auto& w = body["workdays"];
    static const std::vector<std::string> kDays{"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    if (!w.isArray()) throw ApiError::validation({{"/workdays", "must be an array"}});
    std::string arr = "{";
    std::set<std::string> seen;
    for (Json::ArrayIndex i = 0; i < w.size(); ++i) {
      if (!w[i].isString() || std::find(kDays.begin(), kDays.end(), w[i].asString()) == kDays.end()) {
        throw ApiError::validation({{"/workdays/" + std::to_string(i), "must be one of MO..SU"}});
      }
      if (!seen.insert(w[i].asString()).second) {
        throw ApiError::validation({{"/workdays/" + std::to_string(i), "is listed twice"}});
      }
      arr += (i ? "," : "") + w[i].asString();
    }
    workdays = arr + "}";
  }

  auto tx = co_await TenantTx::begin(ctx.who());
  bool badTimezone = false;
  try {
    const std::vector<std::optional<std::string>> params{
        ctx.who().userId, displayName ? std::optional<std::string>(*displayName) : std::nullopt, timezone,
        dayStart ? std::optional<std::string>(std::to_string(*dayStart)) : std::nullopt, workdays};
    co_await tx.execParams(
        "UPDATE users SET display_name = coalesce($2, display_name), timezone = coalesce($3, timezone), "
        "day_start_hour = coalesce($4::smallint, day_start_hour), workdays = coalesce($5::text[], workdays) "
        "WHERE id = $1::uuid",
        params);
  } catch (const drogon::orm::DrogonDbException& e) {
    // check_user_timezone() raises check_violation for unknown IANA zones.
    badTimezone = std::string(e.base().what()).find("invalid timezone") != std::string::npos;
    if (!badTimezone) throw;
  }
  if (badTimezone) throw ApiError::validation({{"/timezone", "unknown IANA timezone"}});
  auto user = co_await loadUser(tx, ctx.who().userId);
  co_await tx.commit();
  co_return jsonResponse(user);
}

Task<drogon::HttpResponsePtr> changePassword(RequestContext& ctx) {
  const auto body = ctx.jsonBody();
  BodyReader in{body, {"current_password", "new_password"}};
  in.rejectUnknown();
  const auto current = *in.string("current_password", 1, 1024, true);
  const auto next = *in.string("new_password", 12, 1024, true);

  auto tx = co_await TenantTx::begin(ctx.who());
  const auto rows = co_await tx.exec("SELECT password_hash FROM users WHERE id = $1", ctx.who().userId);
  if (rows.empty()) throw ApiError::notFound();
  const auto stored = rows[0]["password_hash"].as<std::string>();
  const bool ok = co_await runBlocking([current, stored] { return crypto::verifyPassword(current, stored); });
  if (!ok) throw ApiError::validation({{"/current_password", "is incorrect"}});
  const auto newHash = co_await runBlocking([next] { return crypto::hashPassword(next); });
  co_await tx.exec("UPDATE users SET password_hash = $2 WHERE id = $1", ctx.who().userId, newHash);
  co_await tx.exec(
      "UPDATE credentials SET revoked_at = now() WHERE user_id = $1 AND kind = 'user_session' "
      "AND revoked_at IS NULL AND id <> $2",
      ctx.who().userId, ctx.who().credentialId);
  co_await tx.commit();
  co_return noContent();
}

}  // namespace

void registerAuthRoutes() {
  addRoute("/api/v1/auth/login", drogon::Post, Access::Public, login);
  addRoute("/api/v1/auth/logout", drogon::Post, Access::Authenticated, logout);
  addRoute("/api/v1/me", drogon::Get, Access::Authenticated, me);
  addRoute("/api/v1/me", drogon::Patch, Access::Authenticated, updateMe);
  addRoute("/api/v1/me/password", drogon::Post, Access::Authenticated, changePassword);
}

}  // namespace thelife::http
