#pragma once

#include <drogon/utils/coroutine.h>

#include <string>

#include "db/tenant_tx.h"
#include "scoring/engine.h"

// Reads one user-day into engine inputs (scoring_lld.md §3–§5): profile, planned items, evidence, targets,
// consistency history, settings.
namespace thelife::scoring {

struct Profile {
  std::string name;
  bool scored = true;
};

drogon::Task<Profile> profileFor(db::TenantTx& tx, const std::string& userId, const std::string& localDay);
drogon::Task<Settings> loadSettings(db::TenantTx& tx, const std::string& userId);
drogon::Task<DayInputs> gatherDay(db::TenantTx& tx, const std::string& userId, const std::string& localDay);

}  // namespace thelife::scoring
