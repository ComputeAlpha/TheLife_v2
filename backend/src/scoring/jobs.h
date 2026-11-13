#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "db/tenant_tx.h"

namespace thelife::scoring {

// Queues the day's score and the previous day's re-score (late entries), from day.close.
drogon::Task<void> enqueueDayScoring(db::TenantTx& tx, const std::string& userId, const std::string& localDay);
// Queues a week or month score at period close.
drogon::Task<void> enqueuePeriodScoring(db::TenantTx& tx, const std::string& userId, const std::string& period,
                                        const std::string& periodStart);
void registerJobs();

}  // namespace thelife::scoring
