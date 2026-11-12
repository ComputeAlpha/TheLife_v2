#pragma once

#include <optional>
#include <string>

#include "auth/principal.h"
#include "tools/manifest.h"

namespace thelife::tools {

enum class Decision { Allow, RequireApproval, Propose, Deny };

// Provenance of the entity a tool targets (update/delete), read before deciding.
struct TargetProvenance {
  std::string createdByKind;  // principal_kind
  std::string origin;         // origin_kind
};

struct PolicyInput {
  const auth::Principal& principal;
  const ToolDef& tool;
  std::optional<TargetProvenance> target;
  bool agentWritesEnabled = true;  // tenants.agent_writes_enabled (kill switch)
  bool mcpClient = false;          // api_key used by an MCP client (Stage 3)
};

// Permission table of docs/tool_registry.md §4. Principal-kind and scope checks happen before this.
Decision decide(const PolicyInput& in);

}  // namespace thelife::tools
