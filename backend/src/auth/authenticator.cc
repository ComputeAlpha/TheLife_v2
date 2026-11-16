#include "auth/authenticator.h"

#include <string>

#include "core/crypto.h"
#include "db/database.h"

namespace thelife::auth {

namespace {
std::vector<std::string> parsePgTextArray(const std::string& s) {
  // Scope values match ^[a-z_]+:(read|write)$, so no quoting or escaping occurs.
  std::vector<std::string> out;
  if (s.size() < 2) return out;
  std::string cur;
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    if (s[i] == ',') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(s[i]);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
}  // namespace

drogon::Task<std::optional<Principal>> authenticateToken(std::string_view token) {
  const auto hash = crypto::hashPresentedToken(token);
  if (hash.empty()) co_return std::nullopt;
  const auto& client = db::Database::instance().client();
  if (!client) co_return std::nullopt;

  const auto hex = crypto::pgByteaHex(hash);
  const auto rows = co_await client->execSqlCoro(
      "SELECT credential_id, tenant_id, kind::text AS kind, user_id, device_id, scopes::text AS scopes "
      "FROM auth.authenticate($1::bytea)",
      hex);
  if (rows.empty()) co_return std::nullopt;

  const auto& r = rows[0];
  Principal p;
  const auto kind = r["kind"].as<std::string>();
  p.kind = kind == "device" ? PrincipalKind::Device : kind == "api_key" ? PrincipalKind::ApiKey : PrincipalKind::User;
  p.credentialId = r["credential_id"].as<std::string>();
  p.tenantId = r["tenant_id"].as<std::string>();
  p.userId = r["user_id"].as<std::string>();
  if (!r["device_id"].isNull()) p.deviceId = r["device_id"].as<std::string>();
  p.scopes = parsePgTextArray(r["scopes"].as<std::string>());
  if (p.kind == PrincipalKind::Device) {
    // Device tokens carry a fixed scope set regardless of what is stored (collector_lld.md §8).
    p.scopes = {"ingest:write", "devices:self", "notifications:read"};
  }
  co_return p;
}

drogon::Task<std::optional<Principal>> authenticate(const drogon::HttpRequestPtr& req, bool* viaCookie) {
  if (viaCookie) *viaCookie = false;
  const auto& header = req->getHeader("authorization");
  if (header.empty()) {
    const auto& cookie = req->getCookie(kSessionCookie);
    if (cookie.empty()) co_return std::nullopt;
    auto p = co_await authenticateToken(cookie);
    if (!p || p->kind != PrincipalKind::User || !cookie.starts_with("tlu_")) co_return std::nullopt;
    if (viaCookie) *viaCookie = true;
    co_return p;
  }
  constexpr std::string_view kBearer = "Bearer ";
  if (header.size() <= kBearer.size() || !std::string_view(header).starts_with(kBearer)) co_return std::nullopt;
  co_return co_await authenticateToken(std::string_view(header).substr(kBearer.size()));
}

}  // namespace thelife::auth
