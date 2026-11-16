#include "ingest/jobs.h"

#include <trantor/utils/Logger.h>

#include "core/crypto.h"
#include "core/env.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/field_crypt.h"
#include "db/job_store.h"
#include "ingest/derive.h"
#include "ingest/rules.h"
#include "jobs/worker.h"
#include "llm/classifier.h"

namespace thelife::ingest {

namespace {

using drogon::Task;

constexpr int kBatch = 20000;

int64_t epochOf(const std::string& rfc3339) {
  const auto t = llm::parseRfc3339(rfc3339);
  return t ? std::chrono::duration_cast<std::chrono::seconds>(t->time_since_epoch()).count() : 0;
}

// Epoch seconds as a parameter for to_timestamp($n::double precision).
std::string epochText(int64_t epoch) { return std::to_string(epoch); }

}  // namespace

Task<void> enqueueDerive(db::TenantTx& tx, const std::string& deviceId, int delaySec) {
  db::jobs::NewJob j;
  j.kind = "ingest.derive";
  j.payload["device_id"] = deviceId;
  j.dedupeKey = "derive:" + deviceId;
  static const int kDefault = env::getInt("THELIFE_INGEST_DERIVE_DELAY_S", 60, 0, 3600);
  j.delaySec = delaySec < 0 ? kDefault : delaySec;
  j.priority = 3;
  co_await db::jobs::enqueue(tx, j);
}

Task<int> deriveDevice(const std::string& tenantId, const std::string& deviceId) {
  auto tx = co_await db::TenantTx::beginSystem(tenantId);
  const auto dev = co_await tx.exec(
      "SELECT extract(epoch FROM coalesce(s.derived_until, "
      "  (SELECT min(captured_at) FROM observations o WHERE o.device_id = d.id) - interval '1 second', d.created_at))"
      "  ::bigint AS until "
      "FROM devices d LEFT JOIN ingest_state s ON s.device_id = d.id WHERE d.id = $1::uuid",
      deviceId);
  if (dev.empty()) co_return 0;
  const int64_t from = dev[0]["until"].as<int64_t>();

  auto decode = [&](const drogon::orm::Row& r) -> Task<Obs> {
    Obs o;
    o.at = r["at"].as<int64_t>();
    o.kind = r["kind"].as<std::string>();
    o.payload = co_await fieldcrypt::openText(tx, "observations", "payload", r["p"].as<std::string>());
    if (o.kind == "idle" && o.payload["at"].isString()) o.eventAt = epochOf(o.payload["at"].asString());
    co_return o;
  };
  constexpr const char* kCols =
      "extract(epoch FROM captured_at)::bigint AS at, kind, '\\x' || encode(payload, 'hex') AS p ";

  // The state at `from`: the focused window and whether the user was idle.
  std::optional<Window> current;
  bool idle = false;
  const auto lastWin = co_await tx.exec(std::string("SELECT ") + kCols +
                                            "FROM observations WHERE device_id = $1::uuid AND kind IN ('window', 'session') "
                                            "AND captured_at <= to_timestamp($2::double precision) "
                                            "ORDER BY captured_at DESC, client_seq DESC LIMIT 1",
                                        deviceId, epochText(from));
  if (!lastWin.empty()) {
    const auto o = co_await decode(lastWin[0]);
    if (o.kind == "window") current = windowOf(o.payload);
  }
  const auto lastIdle = co_await tx.exec(std::string("SELECT ") + kCols +
                                             "FROM observations WHERE device_id = $1::uuid AND kind = 'idle' "
                                             "AND captured_at <= to_timestamp($2::double precision) "
                                             "ORDER BY captured_at DESC, client_seq DESC LIMIT 1",
                                         deviceId, epochText(from));
  if (!lastIdle.empty()) {
    const auto o = co_await decode(lastIdle[0]);
    const bool counts = !o.payload.get("passive", false).asBool() || o.payload.get("locked", false).asBool();
    idle = o.payload.get("event", "").asString() == "start" && counts;
  }

  const auto rows = co_await tx.exec(std::string("SELECT ") + kCols +
                                         "FROM observations WHERE device_id = $1::uuid "
                                         "AND captured_at > to_timestamp($2::double precision) "
                                         "ORDER BY captured_at, client_seq LIMIT " + std::to_string(kBatch),
                                     deviceId, epochText(from));
  if (rows.empty()) {
    co_await tx.commit();
    co_return 0;
  }
  std::vector<Obs> obs;
  obs.reserve(rows.size());
  for (const auto& r : rows) obs.push_back(co_await decode(r));

  std::vector<Rule> userRules;
  for (const auto& r : co_await tx.exec("SELECT to_jsonb(r)::text AS j FROM ingest_rules r "
                                        "WHERE deleted_at IS NULL AND enabled")) {
    userRules.push_back(ruleFromRow(json::parse(r["j"].as<std::string>())));
  }

  const auto spans = focusSpans(current, idle, from, obs);
  const auto bs = blocks(spans.spans, [&](const Window& w) { return classify(userRules, w); });

  const auto dek = co_await fieldcrypt::dekFor(tx);
  for (const auto& b : bs) {
    const auto summary = crypto::pgByteaHex(fieldcrypt::seal(dek, tenantId, "detections", "summary", summaryOf(b)));
    const auto evidence = crypto::pgByteaHex(fieldcrypt::seal(dek, tenantId, "detections", "evidence", evidenceOf(b)));
    co_await tx.exec(
        "INSERT INTO detections (id, tenant_id, created_by_kind, source, device_id, started_at, ended_at, area, "
        "category, summary, confidence, evidence) VALUES ($1::uuid, $2::uuid, 'system', 'process', $3::uuid, "
        "to_timestamp($4::double precision), to_timestamp($5::double precision), $6::area, $7, $8::bytea, "
        "$9::real, $10::bytea)",
        ids::newUuidV7(), tenantId, deviceId, std::to_string(b.from), std::to_string(b.to), b.c.area, b.c.category,
        summary, std::to_string(b.c.confidence), evidence);
  }
  // Never move backwards; with a full batch, continue from the last observation read.
  int64_t until = std::max(spans.until, from);
  if (static_cast<int>(rows.size()) == kBatch) until = std::max(until, obs.back().at - 1);
  co_await tx.exec(
      "INSERT INTO ingest_state (tenant_id, device_id, derived_until) VALUES ($1::uuid, $2::uuid, "
      "to_timestamp($3::double precision)) ON CONFLICT (device_id) DO UPDATE SET derived_until = "
      "greatest(ingest_state.derived_until, EXCLUDED.derived_until), updated_at = now()",
      tenantId, deviceId, epochText(until));
  if (static_cast<int>(rows.size()) == kBatch) co_await enqueueDerive(tx, deviceId, 1);
  co_await tx.commit();
  co_return static_cast<int>(bs.size());
}

void registerJobs() {
  jobs::registerHandler("ingest.derive", [](const jobs::Job& j) -> Task<void> {
    const int n = co_await deriveDevice(j.tenantId, j.payload["device_id"].asString());
    if (n > 0) LOG_DEBUG << "ingest.derive: " << n << " process detections";
  });
}

}  // namespace thelife::ingest
