#pragma once

#include <json/value.h>

#include <memory>
#include <vector>

#include "core/errors.h"

namespace thelife::tools {

// Compiled JSON Schema (valijson, draft 7 semantics, which cover the subset the contract generator emits) plus
// format checks valijson lacks: uuid and email. Errors carry JSON Pointer paths.
class SchemaValidator {
 public:
  explicit SchemaValidator(const Json::Value& schema);
  ~SchemaValidator();
  SchemaValidator(SchemaValidator&&) noexcept;
  SchemaValidator& operator=(SchemaValidator&&) noexcept;

  std::vector<FieldError> validate(const Json::Value& instance) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thelife::tools
