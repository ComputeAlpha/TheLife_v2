#include "memory/registry.h"

#include "core/ids.h"
#include "core/json.h"

namespace thelife::memory {

namespace {
std::optional<std::string> opt(const std::string& s) { return s.empty() ? std::nullopt : std::optional(s); }
}  // namespace

drogon::Task<std::string> upsertEntity(db::TenantTx& tx, const NewFact& f) {
  const auto found = co_await tx.exec(
      "SELECT id::text AS id FROM kg_entities WHERE user_id = $1::uuid AND deleted_at IS NULL "
      "  AND (lower(name) = lower($2) OR lower($2) = ANY (SELECT lower(a) FROM unnest(aliases) a)) LIMIT 1",
      f.userId, f.subject);
  if (!found.empty()) {
    if (f.pinned) co_await tx.exec("UPDATE kg_entities SET pinned = true WHERE id = $1::uuid", found[0]["id"].as<std::string>());
    co_return found[0]["id"].as<std::string>();
  }
  const auto id = ids::newUuidV7();
  std::vector<std::optional<std::string>> params{id, tx.tenantId(), f.createdByKind, opt(f.createdById), f.origin,
                                                 f.sourceMessageId, f.userId, f.subjectKind, f.subject,
                                                 f.pinned ? "true" : "false"};
  co_await tx.execParams(
      "INSERT INTO kg_entities (id, tenant_id, created_by_kind, created_by_id, origin, source_message_id, user_id, "
      "kind, name, pinned, last_used_at) VALUES ($1::uuid, $2::uuid, $3::principal_kind, $4::uuid, $5::origin_kind, "
      "$6::uuid, $7::uuid, $8, $9, $10::boolean, now())",
      std::move(params));
  co_return id;
}

drogon::Task<std::string> addFact(db::TenantTx& tx, NewFact f) {
  const auto entity = co_await upsertEntity(tx, f);
  const auto dup = co_await tx.exec(
      "SELECT id::text AS id FROM kg_facts WHERE subject_id = $1::uuid AND deleted_at IS NULL "
      "  AND status IN ('active', 'proposed') AND lower(statement) = lower($2) LIMIT 1",
      entity, f.statement);
  if (!dup.empty()) co_return dup[0]["id"].as<std::string>();
  const auto id = ids::newUuidV7();
  std::vector<std::optional<std::string>> params{
      id, tx.tenantId(), f.createdByKind, opt(f.createdById), f.origin, f.sourceMessageId, f.userId, entity,
      f.predicate, f.value, f.statement, std::to_string(f.confidence), f.status, json::compact(f.sourceRefs)};
  co_await tx.execParams(
      "INSERT INTO kg_facts (id, tenant_id, created_by_kind, created_by_id, origin, source_message_id, user_id, "
      "subject_id, predicate, value, statement, confidence, status, source_refs) "
      "VALUES ($1::uuid, $2::uuid, $3::principal_kind, $4::uuid, $5::origin_kind, $6::uuid, $7::uuid, $8::uuid, $9, "
      "$10, $11, $12::real, $13, $14::jsonb)",
      std::move(params));
  co_return id;
}

drogon::Task<std::vector<std::string>> profileFacts(db::TenantTx& tx, const std::string& userId, int limit) {
  const auto r = co_await tx.exec(
      "SELECT e.name, f.statement FROM kg_facts f JOIN kg_entities e ON e.id = f.subject_id "
      "WHERE f.user_id = $1::uuid AND f.status = 'active' AND f.deleted_at IS NULL AND e.deleted_at IS NULL "
      "ORDER BY e.pinned DESC, e.salience DESC, coalesce(f.last_used_at, f.created_at) DESC LIMIT $2::int",
      userId, std::to_string(limit));
  std::vector<std::string> out;
  for (const auto& row : r) out.push_back(row["statement"].as<std::string>());
  co_return out;
}

drogon::Task<std::vector<std::string>> factsMentionedIn(db::TenantTx& tx, const std::string& userId,
                                                        const std::string& text, int limit) {
  if (text.empty()) co_return std::vector<std::string>{};
  const auto r = co_await tx.exec(
      "SELECT e.name, f.statement FROM kg_entities e JOIN kg_facts f ON f.subject_id = e.id "
      "WHERE e.user_id = $1::uuid AND e.deleted_at IS NULL AND e.kind <> 'self' AND f.status = 'active' "
      "  AND f.deleted_at IS NULL AND length(e.name) >= 3 "
      "  AND ($2 ILIKE '%' || e.name || '%' OR EXISTS (SELECT 1 FROM unnest(e.aliases) a "
      "                                               WHERE length(a) >= 3 AND $2 ILIKE '%' || a || '%')) "
      "ORDER BY e.salience DESC, f.created_at DESC LIMIT $3::int",
      userId, text, std::to_string(limit));
  std::vector<std::string> out;
  for (const auto& row : r) out.push_back(row["name"].as<std::string>() + ": " + row["statement"].as<std::string>());
  co_return out;
}

drogon::Task<void> touch(db::TenantTx& tx, const std::vector<std::string>& factIds) {
  if (factIds.empty()) co_return;
  std::string arr = "{";
  for (std::size_t i = 0; i < factIds.size(); ++i) arr += (i ? "," : "") + factIds[i];
  arr += "}";
  co_await tx.exec("UPDATE kg_facts SET last_used_at = now() WHERE id = ANY($1::uuid[])", arr);
  co_await tx.exec(
      "UPDATE kg_entities SET last_used_at = now(), salience = least(10, salience + 0.1) "
      "WHERE id IN (SELECT subject_id FROM kg_facts WHERE id = ANY($1::uuid[]))",
      arr);
}

}  // namespace thelife::memory
