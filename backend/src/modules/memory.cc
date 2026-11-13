// Memory tools (agent_lld.md §5.1, §6.4): remember, recall, update, forget, history search; and the user's view of
// remembered facts (/memory/facts).
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "crud/resource.h"
#include "memory/embedder.h"
#include "memory/registry.h"
#include "memory/semantic.h"

#include <algorithm>
#include <map>
#include "modules/modules.h"

namespace thelife::modules {

namespace {

using drogon::Task;
using tools::ToolCall;

std::vector<std::string> factProjection() { return crud::projectionFor(tools::registry(), "memory_facts"); }

Task<Json::Value> remember(ToolCall& c) {
  memory::NewFact f;
  f.userId = c.principal.userId;
  f.subject = c.args["subject"].asString();
  f.subjectKind = c.args.get("subject_kind", "self").asString();
  f.statement = c.args["statement"].asString();
  if (c.args.isMember("predicate")) f.predicate = c.args["predicate"].asString();
  if (c.args.isMember("value")) f.value = c.args["value"].asString();
  f.pinned = c.args.get("pinned", false).asBool();
  f.createdByKind = auth::createdByKind(c.principal.kind);
  f.createdById = c.principal.actorId();
  f.origin = c.origin;
  f.sourceMessageId = c.sourceMessageId;
  f.confidence = c.origin == "user_stated" || c.origin == "direct" ? 1.0 : 0.8;
  if (c.sourceMessageId) {
    Json::Value ref;
    ref["type"] = "message";
    ref["id"] = *c.sourceMessageId;
    f.sourceRefs.append(ref);
  }
  const auto id = co_await memory::addFact(c.tx, std::move(f));
  co_return co_await crud::fetchApi(c.tx, "kg_facts", id, factProjection());
}

Task<Json::Value> recall(ToolCall& c) {
  const auto q = c.args["query"].asString();
  const auto rows = co_await c.tx.exec(
      "SELECT f.id::text AS id, e.name AS subject, f.statement, f.confidence, f.origin::text AS origin, "
      "  to_jsonb(f.created_at) #>> '{}' AS created_at "
      "FROM kg_facts f JOIN kg_entities e ON e.id = f.subject_id "
      "WHERE f.user_id = $1::uuid AND f.status = 'active' AND f.deleted_at IS NULL AND e.deleted_at IS NULL "
      "  AND (to_tsvector('simple', f.statement) @@ plainto_tsquery('simple', $2) "
      "       OR e.name ILIKE '%' || $2 || '%' OR (length(e.name) >= 3 AND $2 ILIKE '%' || e.name || '%') "
      "       OR EXISTS (SELECT 1 FROM unnest(e.aliases) a WHERE a ILIKE '%' || $2 || '%')) "
      "ORDER BY e.pinned DESC, ts_rank(to_tsvector('simple', f.statement), plainto_tsquery('simple', $2)) DESC, "
      "  e.salience DESC, f.created_at DESC LIMIT $3::int",
      c.principal.userId, q, std::to_string(c.args.get("limit", 8).asInt()));
  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  std::vector<std::string> used;
  for (const auto& r : rows) {
    Json::Value f;
    f["id"] = r["id"].as<std::string>();
    f["subject"] = r["subject"].as<std::string>();
    f["statement"] = r["statement"].as<std::string>();
    f["confidence"] = r["confidence"].as<double>();
    f["origin"] = r["origin"].as<std::string>();
    f["created_at"] = r["created_at"].as<std::string>();
    used.push_back(f["id"].asString());
    out["items"].append(f);
  }
  // Facts with a similar meaning (embeddings), after the direct matches.
  const int limit = c.args.get("limit", 8).asInt();
  if (static_cast<int>(out["items"].size()) < limit) {
    std::vector<std::string> queryText{q};
    const std::vector<std::string> factKind{"fact"};
    if (const auto e = co_await memory::embed(queryText)) {
      const auto bounds = co_await c.tx.exec("SELECT '-infinity'::timestamptz::text AS f, 'infinity'::timestamptz::text AS t");
      const auto hits = co_await memory::semanticSearch(c.tx, c.principal.userId, e->vectors[0], factKind,
                                                        bounds[0]["f"].as<std::string>(),
                                                        bounds[0]["t"].as<std::string>(), limit);
      for (const auto& h : hits) {
        if (h.score < memory::minRecallScore() || static_cast<int>(out["items"].size()) >= limit) continue;
        if (std::find(used.begin(), used.end(), h.id) != used.end()) continue;
        const auto f = co_await c.tx.exec(
            "SELECT e.name, f.confidence, f.origin::text AS origin FROM kg_facts f JOIN kg_entities e "
            "ON e.id = f.subject_id WHERE f.id = $1::uuid",
            h.id);
        if (f.empty()) continue;
        Json::Value item;
        item["id"] = h.id;
        item["subject"] = f[0]["name"].as<std::string>();
        item["statement"] = h.text;
        item["confidence"] = f[0]["confidence"].as<double>();
        item["origin"] = f[0]["origin"].as<std::string>();
        item["created_at"] = h.at;
        used.push_back(h.id);
        out["items"].append(item);
      }
    }
  }
  co_await memory::touch(c.tx, used);
  co_return out;
}

struct OldFact {
  std::string subjectId, status;
  int version = 0;
};

Task<OldFact> loadFact(ToolCall& c, const std::string& id) {
  const auto r = co_await c.tx.exec(
      "SELECT subject_id::text AS s, status, version FROM kg_facts WHERE id = $1::uuid AND deleted_at IS NULL "
      "FOR UPDATE",
      id);
  if (r.empty()) throw ApiError::notFound();
  OldFact f{r[0]["s"].as<std::string>(), r[0]["status"].as<std::string>(), r[0]["version"].as<int>()};
  if (c.expectedVersion && *c.expectedVersion != f.version) throw ApiError::preconditionFailed();
  co_return f;
}

Task<Json::Value> updateFact(ToolCall& c) {
  const auto oldId = c.args["id"].asString();
  const auto old = co_await loadFact(c, oldId);
  if (old.status == "superseded") throw ApiError::conflict("This fact was already replaced.");
  const auto id = ids::newUuidV7();
  std::vector<std::optional<std::string>> params{
      id, c.principal.tenantId, auth::createdByKind(c.principal.kind), c.principal.actorId(), c.origin,
      c.sourceMessageId, oldId, c.args["statement"].asString(),
      c.args.isMember("confidence") ? std::optional(std::to_string(c.args["confidence"].asDouble())) : std::nullopt};
  co_await c.tx.execParams(
      "INSERT INTO kg_facts (id, tenant_id, created_by_kind, created_by_id, origin, source_message_id, user_id, "
      "subject_id, predicate, object_id, value, statement, confidence, source_refs) "
      "SELECT $1::uuid, $2::uuid, $3::principal_kind, $4::uuid, $5::origin_kind, $6::uuid, o.user_id, o.subject_id, "
      "  o.predicate, o.object_id, NULL, $8, coalesce($9::real, o.confidence), "
      "  jsonb_build_array(jsonb_build_object('type', 'supersedes', 'id', o.id)) "
      "FROM kg_facts o WHERE o.id = $7::uuid",
      std::move(params));
  co_await c.tx.exec(
      "UPDATE kg_facts SET status = 'superseded', superseded_by = $2::uuid, valid_to = now() WHERE id = $1::uuid",
      oldId, id);
  co_return co_await crud::fetchApi(c.tx, "kg_facts", id, factProjection());
}

Task<Json::Value> forget(ToolCall& c) {
  const auto id = c.args["id"].asString();
  co_await loadFact(c, id);
  co_await c.tx.exec("UPDATE kg_facts SET deleted_at = now() WHERE id = $1::uuid", id);
  co_return Json::Value();
}

Task<Json::Value> historySearch(ToolCall& c) {
  const int limit = c.args.get("limit", 10).asInt();
  std::vector<std::optional<std::string>> params{
      c.principal.userId, c.args["query"].asString(),
      c.args.isMember("from") ? std::optional(c.args["from"].asString()) : std::nullopt,
      c.args.isMember("to") ? std::optional(c.args["to"].asString()) : std::nullopt, std::to_string(limit * 2)};
  // Keyword part: Postgres full text search ('simple' configuration, so any language works).
  const auto rows = co_await c.tx.execParams(
      "WITH q AS (SELECT plainto_tsquery('simple', $2) AS q, "
      "                  coalesce($3::timestamptz, now() - interval '3 months') AS f, "
      "                  coalesce($4::timestamptz, now() + interval '1 day') AS t), "
      "hits AS ("
      "  SELECT 'message' AS kind, m.id, m.created_at AS at, c.channel, left(m.content, 400) AS text, "
      "         ts_rank(to_tsvector('simple', m.content), q.q) AS rank "
      "    FROM messages m JOIN conversations c ON c.id = m.conversation_id, q "
      "   WHERE c.user_id = $1::uuid AND m.role IN ('user', 'assistant') "
      "     AND to_tsvector('simple', m.content) @@ q.q AND m.created_at BETWEEN q.f AND q.t "
      "  UNION ALL "
      "  SELECT 'activity', a.id, a.started_at, NULL, left(a.title || coalesce(': ' || a.notes, ''), 400), "
      "         ts_rank(to_tsvector('simple', a.title || ' ' || coalesce(a.notes, '')), q.q) "
      "    FROM activities a, q "
      "   WHERE a.deleted_at IS NULL AND to_tsvector('simple', a.title || ' ' || coalesce(a.notes, '')) @@ q.q "
      "     AND a.started_at BETWEEN q.f AND q.t "
      "  UNION ALL "
      "  SELECT 'day_summary', s.id, s.period_start::timestamptz, s.channel, left(s.content, 600), "
      "         ts_rank(to_tsvector('simple', s.content), q.q) "
      "    FROM episodic_summaries s, q "
      "   WHERE s.user_id = $1::uuid AND to_tsvector('simple', s.content) @@ q.q "
      "     AND s.period_start BETWEEN q.f::date AND q.t::date) "
      "SELECT kind, id::text AS id, to_jsonb(at) #>> '{}' AS at, channel, text, "
      "  (SELECT f::text FROM q) AS f, (SELECT t::text FROM q) AS t "
      "FROM hits ORDER BY rank DESC, at DESC LIMIT $5::int",
      std::move(params));

  // Reciprocal rank fusion of keyword and semantic hits (k = 60).
  struct Item {
    Json::Value v;
    double score = 0;
    bool keyword = false, semantic = false;
  };
  std::map<std::string, Item> merged;
  int rank = 0;
  for (const auto& r : rows) {
    auto& it = merged[r["kind"].as<std::string>() + ":" + r["id"].as<std::string>()];
    it.v["kind"] = r["kind"].as<std::string>();
    it.v["id"] = r["id"].as<std::string>();
    it.v["at"] = r["at"].as<std::string>();
    it.v["channel"] = r["channel"].isNull() ? Json::Value() : Json::Value(r["channel"].as<std::string>());
    it.v["text"] = r["text"].as<std::string>();
    it.keyword = true;
    it.score += 1.0 / (60 + ++rank);
  }
  std::vector<std::string> queryText{c.args["query"].asString()};
  const std::vector<std::string> kinds{"message", "activity", "summary"};
  if (const auto e = co_await memory::embed(queryText)) {
    // The same window as the keyword part.
    std::vector<std::optional<std::string>> window{
        c.args.isMember("from") ? std::optional(c.args["from"].asString()) : std::nullopt,
        c.args.isMember("to") ? std::optional(c.args["to"].asString()) : std::nullopt};
    const auto w = co_await c.tx.execParams(
        "SELECT coalesce($1::timestamptz, now() - interval '3 months')::text AS f, "
        "coalesce($2::timestamptz, now() + interval '1 day')::text AS t",
        std::move(window));
    rank = 0;
    const auto hits = co_await memory::semanticSearch(c.tx, c.principal.userId, e->vectors[0], kinds,
                                                      w[0]["f"].as<std::string>(), w[0]["t"].as<std::string>(),
                                                      limit * 2);
    for (const auto& h : hits) {
      const std::string kind = h.kind == "summary" ? "day_summary" : h.kind;
      auto& it = merged[kind + ":" + h.id];
      if (!it.keyword) {
        it.v["kind"] = kind;
        it.v["id"] = h.id;
        it.v["at"] = h.at;
        it.v["channel"] = h.channel.empty() ? Json::Value() : Json::Value(h.channel);
        it.v["text"] = h.text;
      }
      it.semantic = true;
      it.score += 1.0 / (60 + ++rank);
    }
  }
  std::vector<Item> items;
  for (auto& [_, it] : merged) items.push_back(std::move(it));
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.score > b.score; });
  Json::Value out;
  out["items"] = Json::Value(Json::arrayValue);
  for (auto& it : items) {
    if (static_cast<int>(out["items"].size()) >= limit) break;
    it.v["match"] = it.keyword && it.semantic ? "both" : it.keyword ? "keyword" : "semantic";
    out["items"].append(it.v);
  }
  co_return out;
}

}  // namespace

void bindMemory(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "memory_facts", .table = "kg_facts", .filters = {}, .validate = nullptr});
  const auto facts = crud::targetResolver("kg_facts");
  reg.bind("memory.remember", remember);
  reg.bind("memory.recall", recall);
  reg.bind("memory.update_fact", updateFact, facts);
  reg.bind("memory.forget", forget, facts);
  reg.bind("history.search", historySearch);
}

}  // namespace thelife::modules
