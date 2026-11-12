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
      switch (in.tool.effect) {
        case Effect::Create:
          return Decision::Allow;
        case Effect::Update:
          if (in.target && in.target->createdByKind == "agent" && in.target->origin != "user_stated") {
            return Decision::Allow;
          }
          return Decision::RequireApproval;
        case Effect::Delete:
          return Decision::RequireApproval;
        case Effect::Read:
          return Decision::Allow;
      }
  }
  return Decision::Deny;
}

}  // namespace thelife::tools
