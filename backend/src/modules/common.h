#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/recurrence.h"

#include "db/tenant_tx.h"

namespace thelife::modules {

// The user's local clock: occurrences and calendar days are computed in this zone, with the day starting at
// day_start_hour (a 01:30 session belongs to the previous day when the day starts at 04:00).
struct UserClock {
  std::string timezone;
  int dayStartHour = 4;
  std::string today;  // local date (honouring day_start_hour), YYYY-MM-DD
};
drogon::Task<UserClock> userClock(db::TenantTx& tx, const std::string& userId);

// Converts local wall-clock intervals to instants in `timezone` (ISO 8601 UTC strings), preserving order.
drogon::Task<std::vector<std::pair<std::string, std::string>>> toInstants(
    db::TenantTx& tx, const std::string& timezone,
    const std::vector<std::pair<recurrence::LocalDateTime, recurrence::LocalDateTime>>& local);

// Throws 422 when fields[name] is set and is not an RRULE we accept.
void requireValidRrule(const Json::Value& fields, const std::string& name);

// Overlap filters for resources with a [start, end] interval: from → ends at or after, to → starts before.
std::map<std::string, std::string> intervalFilters(const std::string& startCol, const std::string& endCol);

}  // namespace thelife::modules
