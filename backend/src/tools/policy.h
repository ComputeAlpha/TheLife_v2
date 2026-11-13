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
  int version = 0;            // pinned by approvals (agent_lld.md §5)
  bool recent = false;        // created within the last 24 h (undo window of a chat receipt, §7.4)
};

struct PolicyInput {
  const auth::Principal& principal;
  const ToolDef& tool;
  std::optional<TargetProvenance> target;
  bool agentWritesEnabled = true;  // tenants.agent_writes_enabled (kill switch)
  bool mcpClient = false;          // api_key used by an MCP client (Stage 3)
  bool userTurn = false;           // the agent acts on a message the user just sent (named policies)
};

// Permission table of docs/tool_registry.md §4. Principal-kind and scope checks happen before this.
Decision decide(const PolicyInput& in);

}  // namespace thelife::tools
