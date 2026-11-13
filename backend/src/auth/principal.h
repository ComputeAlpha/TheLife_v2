#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace thelife::auth {

enum class PrincipalKind { User, Device, ApiKey, Agent, System };

inline const char* toString(PrincipalKind k) {
  switch (k) {
    case PrincipalKind::User: return "user";
    case PrincipalKind::Device: return "device";
    case PrincipalKind::ApiKey: return "api_key";
    case PrincipalKind::Agent: return "agent";
    case PrincipalKind::System: return "system";
  }
  return "unknown";
}

// principal_kind value stored in created_by_kind (api_key callers are "external").
inline const char* createdByKind(PrincipalKind k) {
  switch (k) {
    case PrincipalKind::User: return "user";
    case PrincipalKind::Device: return "device";
    case PrincipalKind::ApiKey: return "external";
    case PrincipalKind::Agent: return "agent";
    case PrincipalKind::System: return "system";
  }
  return "system";
}

struct Principal {
  PrincipalKind kind = PrincipalKind::User;
  std::string tenantId;
  std::string userId;        // the human the call acts for (always set for user/device/api_key/agent)
  std::string deviceId;      // device principals only
  std::string credentialId;  // token used for this request (empty for agent/system)
  std::vector<std::string> scopes;  // empty for user sessions: a user may do everything they own

  bool hasScope(const std::string& scope) const {
    if (kind == PrincipalKind::User || kind == PrincipalKind::Agent || kind == PrincipalKind::System) return true;
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
  }
  // id recorded as created_by_id: the device for device calls, otherwise the user.
  const std::string& actorId() const { return kind == PrincipalKind::Device ? deviceId : userId; }
};

}  // namespace thelife::auth
