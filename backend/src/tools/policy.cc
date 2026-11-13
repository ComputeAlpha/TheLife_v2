#include "tools/policy.h"

namespace thelife::tools {

Decision decide(const PolicyInput& in) {
  using auth::PrincipalKind;
  const bool write = in.tool.effect != Effect::Read;
  switch (in.principal.kind) {
    case PrincipalKind::User:
    case PrincipalKind::System:
    case PrincipalKind::Device:
      return Decision::Allow;
    case PrincipalKind::ApiKey:
      return write && in.mcpClient ? Decision::Propose : Decision::Allow;
    case PrincipalKind::Agent:
      if (!write) return Decision::Allow;
      if (!in.agentWritesEnabled) return Decision::Deny;
      // After reading web content, the model's next writes may carry instructions planted in that content: every
      // write of the turn waits for the user (agent_lld.md §5.1, web tools).
      if (in.untrustedContext) return Decision::RequireApproval;
      // Named policy (docs/tool_registry.md §4): logging "done"/"skipped" for a routine is data entry when the
      // user's own message reports it, so it runs (and is audited) instead of waiting for approval.
      if (in.tool.policy == "user_reported_progress" && in.userTurn) return Decision::Allow;
      // Forgetting a fact the user stated needs their approval; inferred facts the agent may drop (spec 5.3).
      if (in.tool.policy == "user_stated_fact" && in.tool.effect == Effect::Delete) {
        return in.target && in.target->origin != "user_stated" ? Decision::Allow : Decision::RequireApproval;
      }
      switch (in.tool.effect) {
        case Effect::Create:
          return Decision::Allow;
        case Effect::Update:
          if (in.target && in.target->createdByKind == "agent" && in.target->origin != "user_stated") {
            return Decision::Allow;
          }
          return Decision::RequireApproval;
        case Effect::Delete:
          // Undo: the agent may remove what it created itself within the last 24 h (receipt "undo", §7.4).
          if (in.target && in.target->createdByKind == "agent" && in.target->recent) return Decision::Allow;
          return Decision::RequireApproval;
        case Effect::Read:
          return Decision::Allow;
      }
  }
  return Decision::Deny;
}

}  // namespace thelife::tools
