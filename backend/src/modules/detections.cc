// Detections (api/resources/detections.yaml). Content is append-only; status moves only through the SQL
// functions app.detection_transition / app.detection_claim, which also guarantee an event row per change
// (migration 0005). This module only calls them and shapes the results.
#include <memory>

#include "core/errors.h"
#include "core/json.h"
#include "core/crypto.h"
#include "crud/resource.h"
#include "db/field_crypt.h"
#include "modules/common.h"
#include "modules/modules.h"
#include "proactive/notifier.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;
using Projection = std::shared_ptr<const std::vector<std::string>>;

std::optional<std::string> opt(const Json::Value& args, const char* name) {
  if (!args.isMember(name) || args[name].isNull()) return std::nullopt;
  return args[name].asString();
}

Task<Json::Value> history(ToolCall& c, const Projection& projection) {
  const auto id = c.args["id"].asString();
  Json::Value out;
  out["detection"] = co_await crud::fetchApi(c.tx, "detections", id, *projection);
  if (out["detection"].isNull()) throw ApiError::notFound();

  out["events"] = Json::Value(Json::arrayValue);
  const auto events = co_await c.tx.exec(
      "SELECT jsonb_build_object('id', e.id, 'detection_id', e.detection_id, 'at', e.at, 'actor_kind', e.actor_kind, "
      "  'from_status', e.from_status, 'to_status', e.to_status, 'reason', e.reason)::text AS j "
      "FROM detection_events e WHERE e.detection_id = $1::uuid ORDER BY e.at, e.id",
      id);
  for (const auto& r : events) out["events"].append(json::parse(r["j"].as<std::string>()));

  // The whole supersede chain this detection belongs to, oldest first (bounded against cycles).
  out["chain"] = Json::Value(Json::arrayValue);
  const auto chain = co_await c.tx.exec(
      "WITH RECURSIVE back(id, prev, depth) AS ("
      "  SELECT d.id, d.supersedes_id, 0 FROM detections d WHERE d.id = $1::uuid "
      "  UNION ALL SELECT d.id, d.supersedes_id, b.depth - 1 FROM detections d JOIN back b ON d.id = b.prev "
      "  WHERE b.depth > -100), "
      "fwd(id, next, depth) AS ("
      "  SELECT d.id, d.superseded_by_id, 0 FROM detections d WHERE d.id = $1::uuid "
      "  UNION ALL SELECT d.id, d.superseded_by_id, f.depth + 1 FROM detections d JOIN fwd f ON d.id = f.next "
      "  WHERE f.depth < 100), "
      "ids AS (SELECT id, depth FROM back UNION SELECT id, depth FROM fwd) "
      "SELECT to_jsonb(d)::text AS j FROM ids JOIN detections d ON d.id = ids.id ORDER BY ids.depth",
      id);
  for (const auto& r : chain) {
    auto row = json::parse(r["j"].as<std::string>());
    co_await fieldcrypt::openRow(c.tx, "detections", row);
    out["chain"].append(crud::toApi(row, *projection));
  }
  co_return out;
}

Task<Json::Value> transition(ToolCall& c, const Projection& projection, const char* action) {
  const std::vector<std::optional<std::string>> params{c.args["id"].asString(), std::string(action),
                                                       opt(c.args, "reason")};
  const auto rows = co_await c.tx.execParams(
      "SELECT to_jsonb(d)::text AS j FROM app.detection_transition($1::uuid, $2::text, $3::text) d", params);
  auto row = json::parse(rows[0]["j"].as<std::string>());
  co_await fieldcrypt::openRow(c.tx, "detections", row);
  if (std::string(action) == "dispute") {
    // The agent re-evaluates disputed evidence in a silent review (spec 5.7, agent_lld.md §4.2). The outbox
    // payload carries the category, not the (encrypted) summary; the review reads the detection itself.
    Json::Value p;
    p["reason"] = c.args["reason"].asString();
    p["category"] = row["category"].asString();
    co_await proactive::emit(c.tx, c.principal.userId.empty() ? std::nullopt : std::optional(c.principal.userId),
                             "detection.disputed", "detection", c.args["id"].asString(), p);
  }
  co_return crud::toApi(row, *projection);
}

Task<Json::Value> claim(ToolCall& c, const Projection& projection) {
  const auto dek = co_await fieldcrypt::dekFor(c.tx);
  const auto summary =
      crypto::pgByteaHex(fieldcrypt::seal(dek, c.tx.tenantId(), "detections", "summary", c.args["summary"]));
  const std::vector<std::optional<std::string>> params{
      c.args["id"].asString(), c.args["area"].asString(),  c.args["category"].asString(),
      summary,                 opt(c.args, "started_at"), opt(c.args, "ended_at")};
  const auto rows = co_await c.tx.execParams(
      "SELECT to_jsonb(d)::text AS j "
      "FROM app.detection_claim($1::uuid, $2::text, $3::text, $4::bytea, $5::timestamptz, $6::timestamptz) d",
      params);
  auto row = json::parse(rows[0]["j"].as<std::string>());
  co_await fieldcrypt::openRow(c.tx, "detections", row);
  co_return crud::toApi(row, *projection);
}

}  // namespace

void bindDetections(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "detections",
                       .table = "detections",
                       .filters = intervalFilters("started_at", "ended_at"),
                       .validate = nullptr});
  const Projection p = std::make_shared<const std::vector<std::string>>(crud::projectionFor(reg, "detections"));
  reg.bind("detections.history", [p](ToolCall& c) { return history(c, p); });
  reg.bind("detections.confirm", [p](ToolCall& c) { return transition(c, p, "confirm"); });
  reg.bind("detections.dispute", [p](ToolCall& c) { return transition(c, p, "dispute"); });
  reg.bind("detections.claim", [p](ToolCall& c) { return claim(c, p); });
}

}  // namespace thelife::modules
