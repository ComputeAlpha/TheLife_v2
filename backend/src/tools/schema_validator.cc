#include "tools/schema_validator.h"

#include <valijson/adapters/jsoncpp_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validation_results.hpp>
#include <valijson/validator.hpp>

#include <regex>

#include "core/ids.h"

namespace thelife::tools {

namespace {

std::string pointer(const std::vector<std::string>& context) {
  // valijson context looks like ["<root>", "[title]", "[0]"]; turn it into /title/0.
  std::string out;
  for (size_t i = 1; i < context.size(); ++i) {
    auto part = context[i];
    if (part.size() >= 2 && part.front() == '[' && part.back() == ']') part = part.substr(1, part.size() - 2);
    if (part.size() >= 2 && part.front() == '"' && part.back() == '"') part = part.substr(1, part.size() - 2);
    out += "/" + part;
  }
  return out;
}

bool typeAllows(const Json::Value& schema, const char* t) {
  const auto& type = schema["type"];
  if (type.isString()) return type.asString() == t;
  for (const auto& x : type) {
    if (x.asString() == t) return true;
  }
  return false;
}

void checkFormats(const Json::Value& schema, const Json::Value& v, const std::string& path,
                  std::vector<FieldError>& errs) {
  if (!schema.isObject()) return;
  if (v.isString() && schema.isMember("format")) {
    const auto f = schema["format"].asString();
    static const std::regex kEmail(R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)");
    if (f == "uuid" && !ids::isUuid(v.asString())) errs.push_back({path, "must be a UUID"});
    if (f == "email" && !std::regex_match(v.asString(), kEmail)) errs.push_back({path, "must be an email address"});
  }
  if (v.isObject() && schema["properties"].isObject()) {
    for (const auto& name : v.getMemberNames()) {
      if (schema["properties"].isMember(name)) {
        checkFormats(schema["properties"][name], v[name], path + "/" + name, errs);
      }
    }
  }
  if (v.isArray() && schema["items"].isObject() && typeAllows(schema, "array")) {
    for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
      checkFormats(schema["items"], v[i], path + "/" + std::to_string(i), errs);
    }
  }
}

}  // namespace

struct SchemaValidator::Impl {
  Json::Value source;
  valijson::Schema schema;
};

SchemaValidator::SchemaValidator(const Json::Value& schema) : impl_(std::make_unique<Impl>()) {
  impl_->source = schema;
  valijson::SchemaParser parser(valijson::SchemaParser::kDraft7);
  valijson::adapters::JsonCppAdapter adapter(impl_->source);
  parser.populateSchema(adapter, impl_->schema);
}

SchemaValidator::~SchemaValidator() = default;
SchemaValidator::SchemaValidator(SchemaValidator&&) noexcept = default;
SchemaValidator& SchemaValidator::operator=(SchemaValidator&&) noexcept = default;

std::vector<FieldError> SchemaValidator::validate(const Json::Value& instance) const {
  valijson::Validator validator(valijson::Validator::kStrongTypes);
  valijson::ValidationResults results;
  valijson::adapters::JsonCppAdapter target(instance);
  std::vector<FieldError> errs;
  if (!validator.validate(impl_->schema, target, &results)) {
    valijson::ValidationResults::Error e;
    while (results.popError(e)) {
      // valijson reports a chain (object → property → keyword); keep the most specific entries only.
      const auto path = pointer(e.context);
      if (e.description.find("Failed to validate against schema associated with property") == 0) continue;
      if (e.description.find("Failed to validate item #") == 0) continue;
      // "Object contains a property that could not be validated ...: 'bogus'." → /bogus: unknown property
      static const std::string kExtra = "could not be validated using 'properties' or 'additionalProperties'";
      if (const auto p = e.description.find(kExtra); p != std::string::npos) {
        const auto q1 = e.description.find('\'', p + kExtra.size());
        const auto q2 = q1 == std::string::npos ? q1 : e.description.find('\'', q1 + 1);
        if (q2 != std::string::npos) {
          errs.push_back({path + "/" + e.description.substr(q1 + 1, q2 - q1 - 1), "unknown property"});
          continue;
        }
      }
      errs.push_back({path, e.description});
    }
  }
  checkFormats(impl_->source, instance, "", errs);
  return errs;
}

}  // namespace thelife::tools
