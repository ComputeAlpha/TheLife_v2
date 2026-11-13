#pragma once

#include <json/value.h>

#include <functional>
#include <map>
#include <string>

#include "tools/registry.h"

namespace thelife::crud {

// Binds the standard tools of one contract resource (<prefix>.list/get/create/update/delete, whichever exist)
// to a table whose columns are named exactly like the contract fields. Encrypted columns (fieldcrypt::columns())
// are sealed on write and opened on read here, so callers see plaintext. Postgres does the JSON → column type
// mapping (jsonb_populate_record); responses are projected onto the contract's output schema, so columns that
// are not in the contract (tenant_id, internals) never leave the server.
struct ResourceBinding {
  std::string prefix;  // tool prefix, e.g. "goals"
  std::string table;   // e.g. "goals"
  // List filters needing more than column equality: arg → SQL fragment using $? for the value
  // (e.g. {"from", "r.ended_at >= $?::timestamptz"}). Other filters default to r.<arg> = $?::<column type>.
  std::map<std::string, std::string> filters;
  // Domain validation and normalisation beyond the schema; throw ApiError. May fill in fields (for example a
  // default derived from another field). `creating` distinguishes create from patch.
  std::function<void(Json::Value& fields, bool creating)> validate;
};

void bindCrud(tools::ToolRegistry& reg, ResourceBinding binding);

// Every binding made so far (prefix → table); the isolation suite checks each table against the contract.
const std::vector<std::pair<std::string, std::string>>& boundResources();

// Output properties of a resource (from <prefix>.get, else <prefix>.create).
std::vector<std::string> projectionFor(const tools::ToolRegistry& reg, const std::string& prefix);

// Provenance lookup for agent update/delete policy on rows of `table` (common resource columns required).
tools::TargetResolver targetResolver(const std::string& table);

// Live (not soft-deleted) row of `table` as API JSON, or null when absent (RLS hides other tenants).
drogon::Task<Json::Value> fetchApi(db::TenantTx& tx, const std::string& table, const std::string& id,
                                   const std::vector<std::string>& projection);

// Converts a to_jsonb(row) object into the API shape: created_by {kind,id}, only `projection` keys. Open the
// encrypted columns first (fieldcrypt::openRow).
Json::Value toApi(const Json::Value& row, const std::vector<std::string>& projection);

}  // namespace thelife::crud
