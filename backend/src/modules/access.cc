// Collector devices and API keys (api/resources/devices.yaml, api_keys.yaml): the user-only tools that issue
// and revoke credentials. Tokens are returned once and only their SHA-256 is stored (backend_lld.md §6.1).
#include <memory>
#include <set>

#include "core/crypto.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "crud/resource.h"
#include "modules/modules.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;
using Projection = std::shared_ptr<const std::vector<std::string>>;

// A device token reaches only the collector endpoints (collector_lld.md §8).
constexpr const char* kDeviceScopes = "{ingest:write,devices:self,notifications:read}";

std::string pgArray(const Json::Value& items) {
  std::string out = "{";
  for (Json::ArrayIndex i = 0; i < items.size(); ++i) out += (i ? "," : "") + items[i].asString();
  return out + "}";
}

Task<Json::Value> registerDevice(ToolCall& c, const Projection& projection) {
  const auto id = ids::newUuidV7();
  const auto rows = co_await c.tx.exec(
      "INSERT INTO devices AS r (id, tenant_id, created_by_kind, created_by_id, user_id, name, platform) "
      "VALUES ($1::uuid, $2::uuid, 'user', $3::uuid, $3::uuid, $4, $5) RETURNING to_jsonb(r)::text AS j",
      id, c.principal.tenantId, c.principal.userId, c.args["name"].asString(), c.args["platform"].asString());
  const auto token = crypto::newToken(crypto::TokenKind::Device);
  co_await c.tx.exec(
      "INSERT INTO credentials (id, tenant_id, kind, user_id, device_id, name, token_prefix, token_hash, scopes) "
      "VALUES ($1::uuid, $2::uuid, 'device', $3::uuid, $4::uuid, $5, $6, $7::bytea, $8::text[])",
      ids::newUuidV7(), c.principal.tenantId, c.principal.userId, id, c.args["name"].asString(), token.prefix,
      crypto::pgByteaHex(token.hash), std::string(kDeviceScopes));
  Json::Value out;
  out["device"] = crud::toApi(json::parse(rows[0]["j"].as<std::string>()), *projection);
  out["token"] = token.token;
  co_return out;
}

Task<Json::Value> revokeDevice(ToolCall& c) {
  const auto id = c.args["id"].asString();
  const auto exists = co_await c.tx.exec(
      "SELECT revoked_at IS NOT NULL AS revoked FROM devices WHERE id = $1::uuid AND deleted_at IS NULL FOR UPDATE", id);
  if (exists.empty()) throw ApiError::notFound();
  if (!exists[0]["revoked"].as<bool>()) {
    co_await c.tx.exec("UPDATE devices SET revoked_at = now() WHERE id = $1::uuid", id);
  }
  // Revoking is idempotent; any credential still live for the device dies with it.
  co_await c.tx.exec("UPDATE credentials SET revoked_at = now() WHERE device_id = $1::uuid AND revoked_at IS NULL",
                     id);
  co_return Json::Value();
}

// Scopes an API key may carry: those of tools that api_key principals can call. Anything else (devices,
// api_keys, device-only scopes) is rejected, so a key can never mint keys or act as a collector.
std::set<std::string> grantableScopes(const tools::ToolRegistry& reg) {
  std::set<std::string> out;
  for (const auto& [name, def] : reg.manifest().tools) {
    if (!def.principals.count("api_key")) continue;
    out.insert(def.scopes.begin(), def.scopes.end());
  }
  return out;
}

Task<Json::Value> issueKey(ToolCall& c, const Projection& projection, const std::set<std::string>& grantable) {
  const auto& scopes = c.args["scopes"];
  for (Json::ArrayIndex i = 0; i < scopes.size(); ++i) {
    if (!grantable.count(scopes[i].asString())) {
      throw ApiError::validation({{"/scopes/" + std::to_string(i), "is not a scope an API key can hold"}});
    }
  }
  std::optional<std::string> expires;
  if (c.args.isMember("expires_at") && !c.args["expires_at"].isNull()) {
    expires = c.args["expires_at"].asString();
    const auto future = co_await c.tx.exec("SELECT $1::timestamptz > now() AS ok", *expires);
    if (!future[0]["ok"].as<bool>()) throw ApiError::validation({{"/expires_at", "must be in the future"}});
  }
  const auto id = ids::newUuidV7();
  const auto token = crypto::newToken(crypto::TokenKind::ApiKey);
  const std::vector<std::optional<std::string>> params{
      id, c.principal.tenantId, c.principal.userId, c.args["name"].asString(), token.prefix,
      crypto::pgByteaHex(token.hash), pgArray(scopes), expires};
  co_await c.tx.execParams(
      "INSERT INTO credentials (id, tenant_id, kind, user_id, name, token_prefix, token_hash, scopes, expires_at) "
      "VALUES ($1::uuid, $2::uuid, 'api_key', $3::uuid, $4, $5, $6::bytea, $7::text[], $8::timestamptz)",
      params);
  Json::Value out;
  out["api_key"] = co_await crud::fetchApi(c.tx, "api_keys", id, *projection);
  out["token"] = token.token;
  co_return out;
}

Task<Json::Value> revokeKey(ToolCall& c) {
  const auto id = c.args["id"].asString();
  const auto exists = co_await c.tx.exec("SELECT 1 FROM api_keys WHERE id = $1::uuid", id);
  if (exists.empty()) throw ApiError::notFound();
  co_await c.tx.exec(
      "UPDATE credentials SET revoked_at = now() WHERE id = $1::uuid AND kind = 'api_key' AND revoked_at IS NULL", id);
  co_return Json::Value();
}

}  // namespace

void bindAccess(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "devices", .table = "devices", .filters = {}, .validate = nullptr});
  const Projection devices = std::make_shared<const std::vector<std::string>>(crud::projectionFor(reg, "devices"));
  reg.bind("devices.register", [devices](ToolCall& c) { return registerDevice(c, devices); });
  reg.bind("devices.revoke", revokeDevice, crud::targetResolver("devices"));

  crud::bindCrud(reg, {.prefix = "api_keys", .table = "api_keys", .filters = {}, .validate = nullptr});
  const Projection keys = std::make_shared<const std::vector<std::string>>(crud::projectionFor(reg, "api_keys"));
  auto grantable = std::make_shared<const std::set<std::string>>(grantableScopes(reg));
  reg.bind("api_keys.issue", [keys, grantable](ToolCall& c) { return issueKey(c, keys, *grantable); });
  reg.bind("api_keys.revoke", revokeKey);
}

}  // namespace thelife::modules
