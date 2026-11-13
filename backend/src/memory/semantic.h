#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

#include "db/tenant_tx.h"

// Semantic memory (agent_lld.md §6.2): embeddings of messages, activities, summaries and facts, and vector search
// over them. Everything here degrades to "no semantic results" when the embedder is unavailable.
namespace thelife::memory {

// Queues embeddings.sync for the transaction's tenant (coalesced: one queued or running job per tenant).
drogon::Task<void> enqueueSync(db::TenantTx& tx, int delaySec = 2);

struct SemanticHit {
  std::string kind;  // message | activity | summary | fact
  std::string id, at, channel, text;
  double score = 0;  // cosine similarity
};

// Nearest items of the user, newest-first ties, within [from, to]; kinds limits the owner types.
drogon::Task<std::vector<SemanticHit>> semanticSearch(db::TenantTx& tx, const std::string& userId,
                                                     const std::vector<float>& query,
                                                     const std::vector<std::string>& kinds, const std::string& from,
                                                     const std::string& to, int k);

// Minimum similarity for retrieved context (THELIFE_RECALL_MIN_SCORE). Default 0.50: with bge-m3, related items
// measured 0.47-0.61 and unrelated ones 0.38-0.42 in the Stage 2 live check.
double minRecallScore();

void registerSemanticJobs();

}  // namespace thelife::memory
