#include "db/llm_store.h"

#include <drogon/utils/coroutine.h>

#include "core/ids.h"
#include "db/database.h"
#include "db/tenant_tx.h"

namespace thelife::db {

namespace {

long long epochSeconds(llm::Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

class DbStateStore final : public llm::StateStore {
 public:
  std::vector<llm::ProviderStatus> load() override {
    std::vector<llm::ProviderStatus> out;
    auto& db = Database::instance();
    if (!db.configured()) return out;
    const auto r = db.client()->execSqlSync(
        "SELECT provider, state, extract(epoch FROM until_at)::bigint AS until_s, coalesce(quota_scope, '') AS scope, "
        "coalesce(reason, '') AS reason, extract(epoch FROM changed_at)::bigint AS changed_s FROM ops.provider_state");
    for (const auto& row : r) {
      llm::ProviderStatus s;
      s.provider = row["provider"].as<std::string>();
      s.state = row["state"].as<std::string>();
      if (!row["until_s"].isNull()) s.until = llm::Clock::from_time_t(row["until_s"].as<long long>());
      s.quotaScope = row["scope"].as<std::string>();
      s.reason = row["reason"].as<std::string>();
      s.changedAt = llm::Clock::from_time_t(row["changed_s"].as<long long>());
      out.push_back(std::move(s));
    }
    return out;
  }

  void save(const llm::ProviderStatus& s) override {
    auto& db = Database::instance();
    if (!db.configured()) return;
    const std::string until = s.until ? std::to_string(epochSeconds(*s.until)) : "";
    db.client()->execSqlSync(
        "INSERT INTO ops.provider_state (provider, state, until_at, quota_scope, reason, changed_at) "
        "VALUES ($1, $2, to_timestamp(nullif($3, '')::bigint), nullif($4, ''), nullif($5, ''), now()) "
        "ON CONFLICT (provider) DO UPDATE SET state = EXCLUDED.state, until_at = EXCLUDED.until_at, "
        "quota_scope = EXCLUDED.quota_scope, reason = EXCLUDED.reason, changed_at = now()",
        s.provider, s.state, until, s.quotaScope, s.reason.substr(0, 200));
  }

  llm::Clock::time_point nextWeeklyReset(const llm::WeeklyReset& rule, llm::Clock::time_point now) override {
    auto& db = Database::instance();
    if (!db.configured()) return now + std::chrono::hours(24 * 7);
    // The next <weekday> <hour>:00 local strictly after now, as an instant.
    const auto r = db.client()->execSqlSync(
        "WITH t AS (SELECT to_timestamp($1::bigint) AT TIME ZONE $2 AS local_now), "
        "c AS (SELECT date_trunc('week', local_now) + ($3::int - 1) * interval '1 day' + $4::int * interval '1 hour' "
        "             AS cand, local_now FROM t) "
        "SELECT extract(epoch FROM (CASE WHEN cand <= local_now THEN cand + interval '7 days' ELSE cand END) "
        "       AT TIME ZONE $2)::bigint AS s FROM c",
        std::to_string(epochSeconds(now)), rule.timezone, rule.isoWeekday, rule.hour);
    return llm::Clock::from_time_t(r[0]["s"].as<long long>());
  }
};

class DbUsageSink final : public llm::UsageSink {
 public:
  void record(const llm::CallRecord& c) override {
    if (!Database::instance().configured()) return;
    drogon::sync_wait(write(c));
  }

 private:
  static drogon::Task<void> write(llm::CallRecord c) {
    auto tx = co_await TenantTx::beginSystem(c.tenantId);
    const std::string error = c.error ? llm::toString(*c.error) : "";
    std::vector<std::optional<std::string>> callParams{
        ids::newUuidV7(), c.tenantId, c.purpose, c.provider.empty() ? "none" : c.provider,
        c.model.empty() ? "none" : c.model, c.promptId.empty() ? std::nullopt : std::optional(c.promptId),
        c.promptVersion.empty() ? std::nullopt : std::optional(c.promptVersion), llm::toString(c.priority),
        c.conversationId, std::to_string(c.usage.tokensIn), std::to_string(c.usage.tokensOut),
        std::to_string(c.images), std::to_string(c.attempts), std::to_string(c.latencyMs),
        c.error ? "error" : "ok", c.error ? std::optional(error) : std::nullopt};
    std::vector<std::optional<std::string>> usageParams{
        c.tenantId, c.purpose, c.error ? "1" : "0", std::to_string(c.usage.tokensIn),
        std::to_string(c.usage.tokensOut), std::to_string(c.images)};
    co_await tx.execParams(
        "INSERT INTO llm_calls (id, tenant_id, purpose, provider, model, prompt_id, prompt_version, priority, "
        "conversation_id, tokens_in, tokens_out, images, attempts, latency_ms, status, error_class) "
        "VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, $7, $8, $9::uuid, $10::int, $11::int, $12::int, $13::int, "
        "$14::int, $15, $16)",
        std::move(callParams));
    co_await tx.execParams(
        "INSERT INTO llm_usage (tenant_id, day, purpose, requests, failures, tokens_in, tokens_out, images) "
        "VALUES ($1::uuid, (now() AT TIME ZONE 'UTC')::date, $2, 1, $3::int, $4::bigint, $5::bigint, $6::int) "
        "ON CONFLICT (tenant_id, day, purpose) DO UPDATE SET requests = llm_usage.requests + 1, "
        "failures = llm_usage.failures + EXCLUDED.failures, tokens_in = llm_usage.tokens_in + EXCLUDED.tokens_in, "
        "tokens_out = llm_usage.tokens_out + EXCLUDED.tokens_out, images = llm_usage.images + EXCLUDED.images, "
        "updated_at = now()",
        std::move(usageParams));
    co_await tx.commit();
  }
};

}  // namespace

std::shared_ptr<llm::StateStore> makeDbStateStore() { return std::make_shared<DbStateStore>(); }
std::shared_ptr<llm::UsageSink> makeDbUsageSink() { return std::make_shared<DbUsageSink>(); }

}  // namespace thelife::db
