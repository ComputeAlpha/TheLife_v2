#include "tools/registry.h"

#include <cmath>

#include <drogon/orm/Exception.h>

#include <stdexcept>

#include "core/crypto.h"
#include "core/errors.h"
#include "core/json.h"

namespace thelife::tools {

namespace {

const char* principalName(auth::PrincipalKind k) {
  switch (k) {
    case auth::PrincipalKind::User: return "user";
    case auth::PrincipalKind::Device: return "device";
    case auth::PrincipalKind::ApiKey: return "api_key";
    case auth::PrincipalKind::Agent: return "agent";
    case auth::PrincipalKind::System: return "system";
  }
  return "";
}

// Postgres errors caused by the request (not by the server) become 4xx problems.
std::optional<ApiError> mapDbError(const drogon::orm::DrogonDbException& e) {
  const auto* sqlErr = dynamic_cast<const drogon::orm::SqlError*>(&e.base());
  if (!sqlErr) return std::nullopt;
  const std::string state = sqlErr->sqlState();
  const std::string msg = sqlErr->what();
  auto column = [&](const char* marker) {
    // e.g. null value in column "title" of relation "goals" violates not-null constraint
    const auto p = msg.find(marker);
    if (p == std::string::npos) return std::string();
    const auto start = msg.find('"', p);
    const auto end = msg.find('"', start + 1);
    return start == std::string::npos || end == std::string::npos ? std::string()
                                                                  : msg.substr(start + 1, end - start - 1);
  };
  // Errors raised by our own SQL functions and triggers (SQLSTATE class TL) carry a client-safe message.
  if (state.size() == 5 && state.compare(0, 2, "TL") == 0) {
    std::string text = msg;
    if (text.rfind("ERROR:", 0) == 0) text.erase(0, 6);
    text.erase(0, text.find_first_not_of(" \t"));
    if (const auto nl = text.find('\n'); nl != std::string::npos) text.erase(nl);
    if (state == "TL404") return ApiError::notFound(text);
    if (state == "TL409") return ApiError::conflict(text);
    return ApiError::validation({{"", text}});
  }
  if (state == "23502") {
    const auto c = column("column");
    return ApiError::validation({{"/" + c, "is required"}});
  }
  // Class 22 (data exception): input the schema allowed but Postgres cannot represent. Never a server error.
  if (state == "22009") return ApiError::validation({{"", "a timestamp's UTC offset must be within ±15:59"}});
  if (state == "22P05") return ApiError::validation({{"", "text must not contain NUL (\\u0000) characters"}});
  if (state == "23514" || state.compare(0, 2, "22") == 0) {
    return ApiError::validation({{"", "a value is out of range or inconsistent with related fields"}});
  }
  if (state == "23503") return ApiError::validation({{"", "references an item that does not exist"}});
  if (state == "23505") return ApiError::conflict("An item with the same unique value already exists.");
  return std::nullopt;
}

std::string requestHash(const std::string& tool, const Json::Value& args, std::optional<int> v) {
  const auto canon = tool + "\n" + json::compact(args) + "\n" + (v ? std::to_string(*v) : "-");
  return crypto::pgByteaHex(crypto::sha256(canon));
}

}  // namespace

// JSON Schema counts 8246.0 as an integer; Postgres does not. Integral reals become integers before
// validation, so every layer below sees one representation.
static void normalizeNumbers(Json::Value& v) {
  if (v.isObject() || v.isArray()) {
    for (auto& child : v) normalizeNumbers(child);
  } else if (v.type() == Json::realValue) {
    const double d = v.asDouble();
    if (std::trunc(d) == d && std::abs(d) < 9.0e15) v = Json::Value(static_cast<Json::Int64>(d));
  }
}

void validateArgs(const SchemaValidator& v, const Json::Value& args) {
  auto errs = v.validate(args);
  if (!errs.empty()) throw ApiError::validation(std::move(errs));
}

ToolRegistry::ToolRegistry(const Manifest& manifest) : manifest_(manifest) {}

const ToolDef& ToolRegistry::tool(const std::string& name) const {
  auto it = manifest_.tools.find(name);
  if (it == manifest_.tools.end()) throw std::out_of_range("unknown tool " + name);
  return it->second;
}

void ToolRegistry::bind(const std::string& name, ToolHandler handler, TargetResolver resolver) {
  const auto& def = tool(name);  // throws for unknown names
  if (bindings_.count(name)) throw std::logic_error("tool bound twice: " + name);
  bindings_[name] = Binding{std::move(handler), std::move(resolver), std::make_shared<SchemaValidator>(def.inputSchema)};
}

void ToolRegistry::verifyComplete(int stage) const {
  std::string missing;
  for (const auto& [name, def] : manifest_.tools) {
    if (def.stage <= stage && !bindings_.count(name)) missing += " " + name;
  }
  if (!missing.empty()) throw std::runtime_error("tools without handlers:" + missing);
}

drogon::Task<InvokeResult> ToolRegistry::invoke(const std::string& name, const auth::Principal& principal,
                                                Json::Value args, InvokeOptions opts) {
  auto it = bindings_.find(name);
  if (it == bindings_.end()) throw ApiError::notFound("Unknown operation.");
  const auto& def = tool(name);
  const auto& binding = it->second;

  // 1. who may call it at all
  if (!def.principals.count(principalName(principal.kind))) {
    throw ApiError::forbidden("This operation is not available to this kind of token.");
  }
  for (const auto& scope : def.scopes) {
    if (!principal.hasScope(scope)) throw ApiError::forbidden("Token lacks scope " + scope + ".");
  }

  // 2. arguments
  normalizeNumbers(args);
  validateArgs(*binding.validator, args);
  std::optional<int> expectedVersion;
  if (args.isMember("expected_version")) {
    expectedVersion = args["expected_version"].asInt();
    args.removeMember("expected_version");
  }

  // Responses carrying a freshly issued secret (device token, API key) are never stored for replay: the
  // idempotency store would otherwise hold the plaintext token for 24 h.
  if (opts.idempotencyKey && def.outputSchema["properties"].isMember("token")) {
    throw ApiError::validation({{"Idempotency-Key", "is not supported for operations that issue a secret"}});
  }

  auto tx = co_await db::TenantTx::begin(principal);

  // 3. idempotency: claim the key first; a concurrent duplicate blocks here until the first commits.
  std::string hash;
  if (opts.idempotencyKey) {
    hash = requestHash(name, args, expectedVersion);
    const auto claimed = co_await tx.exec(
        "INSERT INTO idempotency_keys (tenant_id, principal_id, key, request_hash) VALUES ($1, $2, $3, $4::bytea) "
        "ON CONFLICT DO NOTHING RETURNING key",
        principal.tenantId, principal.actorId(), *opts.idempotencyKey, hash);
    if (claimed.empty()) {
      const auto prev = co_await tx.exec(
          "SELECT request_hash = $4::bytea AS same, status_code, response::text AS response FROM idempotency_keys "
          "WHERE tenant_id = $1 AND principal_id = $2 AND key = $3",
          principal.tenantId, principal.actorId(), *opts.idempotencyKey, hash);
      if (prev.empty() || prev[0]["status_code"].isNull()) {
        throw ApiError::conflict("A request with this Idempotency-Key is still in progress.");
      }
      if (!prev[0]["same"].as<bool>()) {
        throw ApiError::validation({{"Idempotency-Key", "was already used for a different request"}});
      }
      InvokeResult replay;
      replay.status = prev[0]["status_code"].as<int>();
      replay.value = prev[0]["response"].isNull() ? Json::Value() : json::parse(prev[0]["response"].as<std::string>());
      replay.replayed = true;
      co_await tx.commit();
      co_return replay;
    }
  }

  // 4. policy (needs the target's provenance for agent updates)
  std::optional<TargetProvenance> target;
  if (binding.resolver && def.targetResource && args.isMember("id") &&
      principal.kind == auth::PrincipalKind::Agent) {
    target = co_await binding.resolver(tx, args["id"].asString());
  }
  const auto decision = decide(PolicyInput{principal, def, target, true, false});
  if (decision == Decision::Deny) throw ApiError::forbidden("Agent writes are disabled for this tenant.");
  if (decision != Decision::Allow) {
    // Approval queue and proposals arrive in Stage 2/3 (agent_lld.md §5, spec 5.6).
    throw ApiError{501, "not-implemented", "Not implemented", "Approvals and proposals are not available yet."};
  }

  // 5. execute
  ToolCall call{def, principal, tx, std::move(args), expectedVersion, opts.requestId};
  InvokeResult result;
  std::optional<ApiError> mapped;
  try {
    result.value = co_await binding.handler(call);
  } catch (const drogon::orm::DrogonDbException& e) {
    mapped = mapDbError(e);
    if (!mapped) throw;
  }
  if (mapped) throw *mapped;
  result.status = def.rest ? def.rest->successStatus : 200;

  if (opts.idempotencyKey) {
    const std::vector<std::optional<std::string>> params{
        principal.tenantId, principal.actorId(), *opts.idempotencyKey, std::to_string(result.status),
        result.value.isNull() ? std::nullopt : std::optional<std::string>(json::compact(result.value))};
    co_await tx.execParams(
        "UPDATE idempotency_keys SET status_code = $4::int, response = $5::jsonb "
        "WHERE tenant_id = $1::uuid AND principal_id = $2::uuid AND key = $3",
        params);
  }
  co_await tx.commit();
  co_return result;
}

ToolRegistry& registry() {
  static ToolRegistry r(embeddedManifest());
  return r;
}

}  // namespace thelife::tools
