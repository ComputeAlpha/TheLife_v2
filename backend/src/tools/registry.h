#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "auth/principal.h"
#include "db/tenant_tx.h"
#include "tools/manifest.h"
#include "tools/policy.h"
#include "tools/schema_validator.h"

namespace thelife::tools {

struct ToolCall {
  const ToolDef& tool;
  const auth::Principal& principal;
  db::TenantTx& tx;
  Json::Value args;                     // validated against tool.inputSchema, expected_version removed
  std::optional<int> expectedVersion;   // If-Match (REST) or expected_version (agent)
  std::string requestId;
};

using ToolHandler = std::function<drogon::Task<Json::Value>(ToolCall&)>;
// Reads the provenance of the target entity (for agent update policy). Return nullopt when it does not exist;
// the handler then answers 404.
using TargetResolver = std::function<drogon::Task<std::optional<TargetProvenance>>(db::TenantTx&, const std::string& id)>;

enum class Outcome { Executed, PendingApproval, Proposed };

struct InvokeResult {
  Outcome outcome = Outcome::Executed;
  int status = 200;    // HTTP status to use (success_status, or the replayed one)
  Json::Value value;   // handler result, or PendingDecision
  bool replayed = false;
};

struct InvokeOptions {
  std::string requestId;
  std::optional<std::string> idempotencyKey;
};

// The single gate every capability passes through, from REST today and from the agent and MCP later
// (docs/tool_registry.md §3–5).
class ToolRegistry {
 public:
  explicit ToolRegistry(const Manifest& manifest);

  void bind(const std::string& name, ToolHandler handler, TargetResolver resolver = nullptr);
  // Fails (throws) if a tool up to `stage` that needs a handler has none, or a binding names an unknown tool.
  void verifyComplete(int stage) const;

  const Manifest& manifest() const { return manifest_; }
  bool isBound(const std::string& name) const { return bindings_.count(name) > 0; }
  const ToolDef& tool(const std::string& name) const;

  drogon::Task<InvokeResult> invoke(const std::string& name, const auth::Principal& principal, Json::Value args,
                                    InvokeOptions opts);

 private:
  struct Binding {
    ToolHandler handler;
    TargetResolver resolver;
    std::shared_ptr<SchemaValidator> validator;
  };
  const Manifest& manifest_;
  std::map<std::string, Binding> bindings_;
};

ToolRegistry& registry();  // process-wide instance over the embedded manifest

// Throws ApiError 422 when args fail the schema (exposed for tests and the agent loop).
void validateArgs(const SchemaValidator& v, const Json::Value& args);

}  // namespace thelife::tools
