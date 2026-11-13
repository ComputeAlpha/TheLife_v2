#include "memory/semantic.h"

#include <drogon/drogon.h>

#include "core/env.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/field_crypt.h"
#include "db/job_store.h"
#include "jobs/worker.h"
#include "memory/embedder.h"

namespace thelife::memory {

namespace {

using drogon::Task;

constexpr std::size_t kChunkChars = 1500;  // about 400 tokens: inside the embedder's 512-token window
constexpr int kBatch = 64;
constexpr int kRoundsPerRun = 4;

std::string knownModelVersion;  // from the latest embed response; rows of another version are re-embedded

std::vector<std::string> chunks(const std::string& text) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < text.size() && out.size() < 8) {
    std::size_t end = std::min(text.size(), pos + kChunkChars);
    if (end < text.size()) {
      const auto ws = text.rfind(' ', end);
      if (ws != std::string::npos && ws > pos + kChunkChars / 2) end = ws;
      while (end > pos && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;  // UTF-8 boundary
    }
    out.push_back(text.substr(pos, end - pos));
    pos = end;
    while (pos < text.size() && text[pos] == ' ') ++pos;
  }
  return out;
}

struct Candidate {
  std::string type, id, text, hash;
  std::string notes;  // activities: the encrypted notes ("\\x..."), appended to the text once opened
};

// One round: what is missing or stale, embedded outside any transaction, then written. Returns how many it saw.
Task<int> syncRound(const std::string& tenant) {
  std::vector<Candidate> todo;
  {
    auto tx = co_await db::TenantTx::beginSystem(tenant);
    const auto rows = co_await tx.exec(
        "WITH src AS ("
        "  SELECT 'message' AS t, m.id, m.content AS text, NULL::bytea AS enc FROM messages m "
        "   WHERE m.role IN ('user', 'assistant') AND m.kind = 'normal' AND length(m.content) >= 12 "
        "     AND m.created_at > now() - interval '90 days' "
        "  UNION ALL SELECT 'activity', a.id, a.title, a.notes FROM activities a "
        "   WHERE a.deleted_at IS NULL "
        "  UNION ALL SELECT 'summary', s.id, s.content, NULL FROM episodic_summaries s "
        "  UNION ALL SELECT 'fact', f.id, f.statement, NULL FROM kg_facts f WHERE f.status = 'active' AND f.deleted_at IS NULL), "
        // The hash of encrypted notes covers their ciphertext: any change (even a reseal) embeds the owner again.
        "hashed AS (SELECT src.*, md5(src.text || coalesce(encode(src.enc, 'hex'), '')) AS h FROM src) "
        "SELECT src.t, src.id::text AS id, src.text, src.h, coalesce('\\x' || encode(src.enc, 'hex'), '') AS enc "
        "FROM hashed src "
        "WHERE NOT EXISTS (SELECT 1 FROM embeddings e WHERE e.owner_type = src.t AND e.owner_id = src.id "
        "                  AND e.chunk_ix = 0 AND e.content_hash = src.h "
        "                  AND ($1 = '' OR e.model_version = $1)) "
        "LIMIT $2::int",
        knownModelVersion, std::to_string(kBatch));
    for (const auto& r : rows) {
      Candidate c{r["t"].as<std::string>(), r["id"].as<std::string>(), r["text"].as<std::string>(),
                  r["h"].as<std::string>(), r["enc"].as<std::string>()};
      if (!c.notes.empty()) {
        c.text += ". " + (co_await fieldcrypt::openText(tx, "activities", "notes", c.notes)).asString();
      }
      todo.push_back(std::move(c));
    }
    co_await tx.commit();
  }
  if (todo.empty()) co_return 0;

  std::vector<std::string> texts;
  std::vector<std::pair<std::size_t, int>> owner;  // text index → (candidate, chunk)
  for (std::size_t i = 0; i < todo.size(); ++i) {
    const auto parts = chunks(todo[i].text);
    for (std::size_t c = 0; c < parts.size(); ++c) {
      texts.push_back(parts[c]);
      owner.emplace_back(i, static_cast<int>(c));
    }
  }
  const auto res = co_await embed(texts);
  if (!res) throw jobs::Defer(std::chrono::system_clock::now() + std::chrono::minutes(5), "embedder unavailable");
  knownModelVersion = res->modelVersion;

  auto tx = co_await db::TenantTx::beginSystem(tenant);
  for (const auto& c : todo) {
    co_await tx.exec("DELETE FROM embeddings WHERE owner_type = $1 AND owner_id = $2::uuid", c.type, c.id);
  }
  for (std::size_t k = 0; k < texts.size(); ++k) {
    const auto& c = todo[owner[k].first];
    co_await tx.exec(
        "INSERT INTO embeddings (id, tenant_id, owner_type, owner_id, chunk_ix, content_hash, model, model_version, "
        "embedding) VALUES ($1::uuid, $2::uuid, $3, $4::uuid, $5::smallint, $6, $7, $8, $9::vector)",
        ids::newUuidV7(), tenant, c.type, c.id, std::to_string(owner[k].second), c.hash, res->model,
        res->modelVersion, vectorLiteral(res->vectors[k]));
  }
  co_await tx.commit();
  co_return static_cast<int>(todo.size());
}

Task<void> sync(const jobs::Job& job) {
  for (int round = 0; round < kRoundsPerRun; ++round) {
    if (co_await syncRound(job.tenantId) < kBatch) co_return;
  }
  // More to do: continue in a fresh job (keeps a large backlog from holding a worker slot).
  db::jobs::NewJob next;
  next.kind = "embeddings.sync";
  next.dedupeKey = "tenant:" + job.tenantId;
  next.delaySec = 1;
  co_await db::jobs::enqueueSystem(job.tenantId, next);
}

}  // namespace

std::string utf8Prefix(const std::string& text, std::size_t maxChars) {
  std::size_t chars = 0, i = 0;
  for (; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80 && chars++ == maxChars) break;
  }
  return text.substr(0, i);
}

double minRecallScore() {
  static const double v = [] {
    const auto s = env::get("THELIFE_RECALL_MIN_SCORE");
    return s.empty() ? 0.50 : std::stod(s);
  }();
  return v;
}

Task<void> enqueueSync(db::TenantTx& tx, int delaySec) {
  if (!embedderConfigured()) co_return;
  db::jobs::NewJob j;
  j.kind = "embeddings.sync";
  j.dedupeKey = "tenant:" + tx.tenantId();
  j.delaySec = delaySec;
  j.priority = 3;
  co_await db::jobs::enqueue(tx, j);
}

Task<std::vector<SemanticHit>> semanticSearch(db::TenantTx& tx, const std::string& userId,
                                              const std::vector<float>& query, const std::vector<std::string>& kinds,
                                              const std::string& from, const std::string& to, int k) {
  std::string arr = "{";
  for (std::size_t i = 0; i < kinds.size(); ++i) arr += (i ? "," : "") + kinds[i];
  arr += "}";
  // RLS filters after the HNSW scan; relaxed_order keeps scanning until enough rows survive (backend_lld §4.5).
  co_await tx.exec("SET LOCAL hnsw.iterative_scan = relaxed_order");
  const auto rows = co_await tx.exec(
      "WITH near AS ("
      "  SELECT owner_type, owner_id, min(d) AS d FROM ("
      "    SELECT owner_type, owner_id, embedding <=> $1::vector AS d FROM embeddings "
      "     WHERE owner_type = ANY($2::text[]) ORDER BY embedding <=> $1::vector LIMIT 80) x "
      "  GROUP BY owner_type, owner_id) "
      "SELECT near.owner_type AS kind, near.owner_id::text AS id, 1 - near.d AS score, "
      "  to_jsonb(coalesce(m.created_at, a.started_at, s.period_start::timestamptz, f.created_at)) #>> '{}' AS at, "
      "  coalesce(c.channel, s.channel, '') AS channel, "
      "  left(coalesce(m.content, a.title, s.content, f.statement), 400) AS text, "
      "  coalesce('\\x' || encode(a.notes, 'hex'), '') AS enc "
      "FROM near "
      "LEFT JOIN messages m ON near.owner_type = 'message' AND m.id = near.owner_id "
      "LEFT JOIN conversations c ON c.id = m.conversation_id "
      "LEFT JOIN activities a ON near.owner_type = 'activity' AND a.id = near.owner_id AND a.deleted_at IS NULL "
      "LEFT JOIN episodic_summaries s ON near.owner_type = 'summary' AND s.id = near.owner_id "
      "LEFT JOIN kg_facts f ON near.owner_type = 'fact' AND f.id = near.owner_id AND f.status = 'active' "
      "  AND f.deleted_at IS NULL "
      "WHERE ((m.id IS NOT NULL AND c.user_id = $3::uuid) OR a.id IS NOT NULL "
      "       OR (s.id IS NOT NULL AND s.user_id = $3::uuid) OR (f.id IS NOT NULL AND f.user_id = $3::uuid)) "
      "  AND coalesce(m.created_at, a.started_at, s.period_start::timestamptz, f.created_at) "
      "      BETWEEN $4::timestamptz AND $5::timestamptz "
      "ORDER BY near.d LIMIT $6::int",
      vectorLiteral(query), arr, userId, from, to, std::to_string(k));
  std::vector<SemanticHit> out;
  for (const auto& r : rows) {
    std::string text = r["text"].as<std::string>();
    if (const auto enc = r["enc"].as<std::string>(); !enc.empty()) {
      text = utf8Prefix(text + ". " + (co_await fieldcrypt::openText(tx, "activities", "notes", enc)).asString(), 400);
    }
    out.push_back({r["kind"].as<std::string>(), r["id"].as<std::string>(), r["at"].as<std::string>(),
                   r["channel"].as<std::string>(), std::move(text), r["score"].as<double>()});
  }
  co_return out;
}

void registerSemanticJobs() { jobs::registerHandler("embeddings.sync", sync); }

}  // namespace thelife::memory
