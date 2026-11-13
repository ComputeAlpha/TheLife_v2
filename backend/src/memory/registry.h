#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

#include "auth/principal.h"
#include "db/tenant_tx.h"

// Long-term registry (agent_lld.md §6.4): entities and short facts about the user.
namespace thelife::memory {

struct NewFact {
  std::string userId;
  std::string subject, subjectKind = "self";
  std::string statement;
  std::optional<std::string> predicate, value;
  double confidence = 1.0;
  std::string status = "active";  // active | proposed
  bool pinned = false;
  std::string createdByKind = "agent";
  std::string createdById;
  std::string origin = "inferred";
  std::optional<std::string> sourceMessageId;
  Json::Value sourceRefs = Json::Value(Json::arrayValue);
};

// Finds the entity by name or alias (case-insensitive) or creates it; returns its id.
drogon::Task<std::string> upsertEntity(db::TenantTx& tx, const NewFact& f);

// Adds the fact unless the same statement is already active for that subject; returns the fact id (new or
// existing).
drogon::Task<std::string> addFact(db::TenantTx& tx, NewFact f);

// Context blocks for the agent prompt.
// Pinned facts first, then by entity salience and recency (the core profile, §6.1 section 3).
drogon::Task<std::vector<std::string>> profileFacts(db::TenantTx& tx, const std::string& userId, int limit);
// Facts of entities whose name or alias occurs in `text` (section 6, retrieved).
drogon::Task<std::vector<std::string>> factsMentionedIn(db::TenantTx& tx, const std::string& userId,
                                                        const std::string& text, int limit);
// Marks facts as used (recall); raises their entities' salience.
drogon::Task<void> touch(db::TenantTx& tx, const std::vector<std::string>& factIds);

}  // namespace thelife::memory
