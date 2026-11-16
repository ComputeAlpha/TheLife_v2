// Collector endpoints (api/resources/ingest.yaml, devices.yaml self_*; backend_lld.md §7.2): observation upload,
// configuration, heartbeat and token rotation for the calling device, plus the user's classification rules.
// Payloads are validated per kind against api/schemas/observations.json and stored sealed (fieldcrypt).
#include <chrono>

#include <trantor/utils/Logger.h>
#include <map>
#include <memory>
#include <set>
#include <algorithm>

#include "core/crypto.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "crud/resource.h"
#include "db/field_crypt.h"
#include "ingest/config.h"
#include "ingest/jobs.h"
#include "vision/captures.h"
#include "llm/classifier.h"
#include "llm/gateway.h"
#include "modules/modules.h"
#include "tools/schema_validator.h"

namespace thelife::generated {
extern const std::string_view kObservationSchemasJson;
}

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

const std::map<std::string, tools::SchemaValidator>& payloadValidators() {
  static const auto validators = [] {
    std::map<std::string, tools::SchemaValidator> m;
    const auto doc = json::parse(generated::kObservationSchemasJson);
    for (const auto& kind : doc.getMemberNames()) {
      if (kind.rfind('_', 0) == 0) continue;  // _comment
      m.emplace(kind, tools::SchemaValidator(doc[kind]));
    }
    return m;
  }();
  return validators;
}

std::string pgArray(const std::vector<std::string>& items) {
  std::string out = "{";
  for (std::size_t i = 0; i < items.size(); ++i) out += (i ? ",\"" : "\"") + items[i] + "\"";
  return out + "}";
}

Task<Json::Value> uploadObservations(ToolCall& c) {
  const auto& items = c.args["observations"];
  const auto& validators = payloadValidators();
  const auto now = std::chrono::system_clock::now();
  std::vector<FieldError> errors;
  for (Json::ArrayIndex i = 0; i < items.size(); ++i) {
    const auto& o = items[i];
    const std::string at = "/observations/" + std::to_string(i);
    const auto v = validators.find(o["kind"].asString());
    if (v == validators.end()) {
      errors.push_back({at + "/kind", "unknown kind"});
      continue;
    }
    for (auto& e : v->second.validate(o["payload"])) errors.push_back({at + "/payload" + e.path, e.message});
    const auto t = llm::parseRfc3339(o["captured_at"].asString());
    if (t && *t > now + std::chrono::minutes(10)) errors.push_back({at + "/captured_at", "is in the future"});
  }
  if (!errors.empty()) throw ApiError::validation(errors);

  const auto dek = co_await fieldcrypt::dekFor(c.tx);
  std::vector<std::string> idsV, seqs, times, kinds, payloads;
  for (const auto& o : items) {
    idsV.push_back(ids::newUuidV7());
    seqs.push_back(std::to_string(o["client_seq"].asInt64()));
    times.push_back(o["captured_at"].asString());
    kinds.push_back(o["kind"].asString());
    payloads.push_back(
        crypto::hex(fieldcrypt::seal(dek, c.principal.tenantId, "observations", "payload", o["payload"])));
  }
  const auto rows = co_await c.tx.exec(
      "INSERT INTO observations (id, tenant_id, device_id, client_seq, captured_at, kind, payload) "
      "SELECT t.i, $1::uuid, $2::uuid, t.s, t.c, t.k, decode(t.p, 'hex') "
      "FROM unnest($3::uuid[], $4::bigint[], $5::timestamptz[], $6::text[], $7::text[]) AS t(i, s, c, k, p) "
      "ON CONFLICT (tenant_id, device_id, client_seq) DO NOTHING RETURNING client_seq",
      c.principal.tenantId, c.principal.deviceId, pgArray(idsV), pgArray(seqs), pgArray(times), pgArray(kinds),
      pgArray(payloads));
  std::set<int64_t> fresh;
  for (const auto& r : rows) fresh.insert(r["client_seq"].as<int64_t>());
  Json::Value out;
  out["accepted"] = Json::Value(Json::arrayValue);
  out["duplicates"] = Json::Value(Json::arrayValue);
  std::set<int64_t> seen;
  for (const auto& o : items) {
    const auto s = o["client_seq"].asInt64();
    if (fresh.count(s) && seen.insert(s).second) {
      out["accepted"].append(static_cast<Json::Int64>(s));
    } else {
      out["duplicates"].append(static_cast<Json::Int64>(s));
    }
  }
  if (!fresh.empty()) co_await ingest::enqueueDerive(c.tx, c.principal.deviceId);
  co_return out;
}

Task<Json::Value> selfConfig(ToolCall& c) { co_return co_await ingest::deviceConfig(c.tx, c.principal.userId); }

// A capture (backend_lld.md §7.2): checked, then handed to the in-memory vision queue once the request has
// committed. Never stored. 409 config-stale when capture is off (the collector fetches its config again), 503
// when the queue is full (the collector drops the capture).
Task<Json::Value> uploadCapture(ToolCall& c) {
  const auto& ctx = c.args["context"];
  if (!c.attachments || c.attachments->size() != c.args["images"].size() || c.attachments->empty()) {
    throw ApiError::validation({{"/images", "one JPEG part per monitor is required"}});
  }
  const auto t = llm::parseRfc3339(ctx["captured_at"].asString());
  const auto now = std::chrono::system_clock::now();
  if (!t || *t > now + std::chrono::minutes(2) || *t < now - std::chrono::minutes(10)) {
    throw ApiError::validation({{"/context/captured_at", "must be within the last 10 minutes"}});
  }
  const auto cfg = co_await ingest::deviceConfig(c.tx, c.principal.userId);
  if (!cfg["enabled"].asBool()) {
    throw ApiError{409, "config-stale", "Capture is off", "Capture is off for this device; fetch the configuration."};
  }
  if (vision::inUse() >= vision::capacity()) {
    throw ApiError::serviceUnavailable("The vision queue is full; drop this capture.", 60);
  }
  auto cap = std::make_shared<vision::Capture>();
  cap->tenantId = c.principal.tenantId;
  cap->userId = c.principal.userId;
  cap->deviceId = c.principal.deviceId;
  cap->captureId = ids::newUuidV7();
  cap->context = ctx;
  cap->images = std::move(*c.attachments);
  cap->windowSec = cfg["capture_interval_s"].asInt();
  c.afterCommit.push_back([cap] {
    if (!vision::submit(std::move(*cap))) LOG_WARN << "vision: queue filled up; capture dropped";
  });
  Json::Value out;
  out["capture_id"] = cap->captureId;
  co_return out;
}

Task<Json::Value> heartbeat(ToolCall& c) {
  Json::Value status;
  for (const char* k : {"queue_depth", "paused_until", "x_ok", "last_capture_at", "last_error"}) {
    if (c.args.isMember(k)) status[k] = c.args[k];
  }
  if (!status.isMember("paused_until")) status["paused_until"] = Json::Value();
  co_await c.tx.exec(
      "UPDATE devices SET last_seen_at = now(), collector_version = $2, status = $3::jsonb WHERE id = $1::uuid",
      c.principal.deviceId, c.args["version"].asString(), json::compact(status));
  co_return Json::Value();
}

Task<Json::Value> rotateToken(ToolCall& c) {
  const auto old = co_await c.tx.exec(
      "SELECT name, scopes::text AS scopes FROM credentials WHERE id = $1::uuid AND revoked_at IS NULL",
      c.principal.credentialId);
  if (old.empty()) throw ApiError{401, "unauthorized", "Unauthorized", "The token is no longer valid."};
  const auto token = crypto::newToken(crypto::TokenKind::Device);
  co_await c.tx.exec(
      "INSERT INTO credentials (id, tenant_id, kind, user_id, device_id, name, token_prefix, token_hash, scopes) "
      "VALUES ($1::uuid, $2::uuid, 'device', $3::uuid, $4::uuid, $5, $6, $7::bytea, $8::text[])",
      ids::newUuidV7(), c.principal.tenantId, c.principal.userId, c.principal.deviceId,
      old[0]["name"].isNull() ? std::string() : old[0]["name"].as<std::string>(), token.prefix,
      crypto::pgByteaHex(token.hash), old[0]["scopes"].as<std::string>());
  // The old token keeps working for 5 minutes (a rotation interrupted mid-way must not lock the collector out).
  const auto exp = co_await c.tx.exec(
      "UPDATE credentials SET expires_at = least(coalesce(expires_at, 'infinity'), now() + interval '5 minutes') "
      "WHERE id = $1::uuid RETURNING to_jsonb(expires_at) #>> '{}' AS e",
      c.principal.credentialId);
  Json::Value out;
  out["token"] = token.token;
  out["previous_expires_at"] = exp[0]["e"].as<std::string>();
  co_return out;
}

void validateRule(Json::Value& f, bool creating) {
  if (!creating) return;  // a patch that clears every match field is caught by the table's CHECK
  for (const char* k : {"match_wm_class", "match_exe", "match_title"}) {
    if (f[k].isString() && !f[k].asString().empty()) return;
  }
  throw ApiError::validation({{"/match_wm_class", "at least one of match_wm_class, match_exe, match_title"}});
}

}  // namespace

void bindIngest(tools::ToolRegistry& reg) {
  reg.bind("ingest.observations", uploadObservations);
  reg.bind("ingest.captures", uploadCapture);
  reg.bind("devices.self_config", selfConfig);
  reg.bind("devices.self_heartbeat", heartbeat);
  reg.bind("devices.self_rotate_token", rotateToken);
  crud::bindCrud(reg, {.prefix = "ingest_rules", .table = "ingest_rules", .filters = {}, .validate = validateRule});
}

}  // namespace thelife::modules
