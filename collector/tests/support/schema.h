#pragma once

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <valijson/adapters/jsoncpp_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>

#include "core/json.h"

namespace thelife::collector::testing {

// Every payload the collector emits must pass the schema the backend validates against
// (api/schemas/observations.json: one source of truth).
inline bool validates(const std::string& kind, const Json::Value& payload, std::string* why) {
  static const Json::Value doc = [] {
    std::ifstream f(std::string(THELIFE_REPO_ROOT) + "/api/schemas/observations.json");
    std::stringstream ss;
    ss << f.rdbuf();
    return *json::parseStrict(ss.str(), 1 << 20);
  }();
  if (!doc.isMember(kind) || kind.starts_with("_")) {
    *why = "unknown kind";
    return false;
  }
  valijson::Schema schema;
  valijson::SchemaParser parser;
  valijson::adapters::JsonCppAdapter schemaAdapter(doc[kind]);
  parser.populateSchema(schemaAdapter, schema);
  valijson::Validator v;
  valijson::ValidationResults results;
  valijson::adapters::JsonCppAdapter target(payload);
  const bool ok = v.validate(schema, target, &results);
  valijson::ValidationResults::Error e;
  while (results.popError(e)) *why += e.description + "; ";
  return ok;
}

inline void expectValid(const std::string& kind, const Json::Value& payload) {
  std::string why;
  EXPECT_TRUE(validates(kind, payload, &why)) << kind << ": " << json::compact(payload) << " -> " << why;
}

}  // namespace thelife::collector::testing
