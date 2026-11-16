#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "db/tenant_tx.h"

namespace thelife::ingest {

// Registers ingest.derive: turns a device's new observations into focus spans and process detections.
void registerJobs();

// Queues ingest.derive for a device, coalesced per device and delayed (THELIFE_INGEST_DERIVE_DELAY_S, default 60)
// so a burst of uploads is derived once. delaySec < 0 means that default.
drogon::Task<void> enqueueDerive(db::TenantTx& tx, const std::string& deviceId, int delaySec = -1);

// Runs the derivation for one device now (the job body; also used by tests). Returns detections written.
drogon::Task<int> deriveDevice(const std::string& tenantId, const std::string& deviceId);

}  // namespace thelife::ingest
