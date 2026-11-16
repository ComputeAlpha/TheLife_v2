#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <string>

#include "db/tenant_tx.h"

namespace thelife::ingest {

// The collector configuration of a user's devices (devices.yaml DeviceConfig). Capture budget (backend_lld.md
// §7.4): tenants.settings.vision {enabled (true), daily_captures (120), capture_monitors (all), capture_max_px
// (1600)}. The day's captures are spread over the expected active hours (learned from 14 days of process
// detections once 3 days have data, else 10 h); the interval stretches when fewer captures are left than the
// rest of the day needs, and capture is off when none are left or no vision model is available.
drogon::Task<Json::Value> deviceConfig(db::TenantTx& tx, const std::string& userId);

}  // namespace thelife::ingest
