#include "ingest/config.h"

#include <algorithm>

#include "core/json.h"
#include "llm/gateway.h"

namespace thelife::ingest {

using drogon::Task;

// Capture budget (backend_lld.md §7.4): the day's captures spread over the expected active hours; the interval
// stretches when fewer captures are left than the rest of the day needs, and capture stops when none are left.
Task<Json::Value> deviceConfig(db::TenantTx& tx, const std::string& userId) {
  const auto rows = co_await tx.exec(
      "WITH u AS (SELECT u.timezone, u.day_start_hour, "
      "  (date_trunc('day', (now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour)) "
      "     + make_interval(hours => u.day_start_hour)) AT TIME ZONE u.timezone AS day_start "
      "  FROM users u WHERE u.id = $1::uuid) "
      "SELECT coalesce(t.settings -> 'vision', '{}'::jsonb)::text AS vision, "
      "  extract(epoch FROM (u.day_start + interval '1 day') - now())::bigint AS left_s, "
      "  (SELECT count(*) FROM llm_calls l WHERE l.purpose = 'vision.analyze' AND l.created_at >= u.day_start) AS used, "
      "  (SELECT avg(s) FROM (SELECT sum(extract(epoch FROM ended_at - started_at)) AS s FROM detections "
      "     WHERE source = 'process' AND started_at > now() - interval '14 days' "
      "     GROUP BY date_trunc('day', started_at AT TIME ZONE u.timezone) "
      "     HAVING sum(extract(epoch FROM ended_at - started_at)) >= 1800) d) AS active_s, "
      "  (SELECT count(*) FROM (SELECT 1 FROM detections WHERE source = 'process' "
      "     AND started_at > now() - interval '14 days' "
      "     GROUP BY date_trunc('day', started_at AT TIME ZONE u.timezone) "
      "     HAVING sum(extract(epoch FROM ended_at - started_at)) >= 1800) d) AS active_days "
      "FROM tenants t, u",
      userId);
  const auto& r = rows[0];
  const auto vision = json::parse(r["vision"].as<std::string>());
  const int daily = std::clamp(vision.get("daily_captures", 120).asInt(), 0, 2000);
  const int64_t used = r["used"].as<int64_t>();
  // Expected active hours: learned from the last 14 days once 3 days have data, else 10 h.
  int64_t activeS = 10 * 3600;
  if (r["active_days"].as<int64_t>() >= 3 && !r["active_s"].isNull()) {
    activeS = std::clamp<int64_t>(static_cast<int64_t>(r["active_s"].as<double>()), 2 * 3600, 16 * 3600);
  }
  const int64_t remaining = daily - used;
  const bool on = vision.get("enabled", true).asBool() && remaining > 0 && llm::gateway().visionAvailable();
  int64_t interval = daily > 0 ? activeS / daily : 3600;
  if (remaining > 0) {
    const int64_t leftS = std::min(std::max<int64_t>(r["left_s"].as<int64_t>(), 0), activeS);
    interval = std::max(interval, leftS / remaining);
  }
  Json::Value out;
  out["enabled"] = on;
  out["capture_interval_s"] = static_cast<Json::Int64>(std::clamp<int64_t>(interval, 60, 3600));
  out["jitter_pct"] = 20;
  const auto monitors = vision.get("capture_monitors", "all").asString();
  out["capture_monitors"] = monitors == "active" ? "active" : "all";
  out["capture_max_px"] = std::clamp(vision.get("capture_max_px", 1600).asInt(), 640, 2560);
  out["idle_threshold_s"] = 120;
  out["process_interval_s"] = 60;
  co_return out;
}

}  // namespace thelife::ingest
