#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/utils/coroutine.h>

#include <optional>
#include <string_view>

#include "auth/principal.h"

namespace thelife::auth {

// Resolves "Authorization: Bearer <token>" to a Principal through auth.authenticate() (SECURITY DEFINER).
// Returns nullopt for missing, malformed, unknown, revoked or expired tokens. Malformed tokens never reach
// the database.
drogon::Task<std::optional<Principal>> authenticate(const drogon::HttpRequestPtr& req);
drogon::Task<std::optional<Principal>> authenticateToken(std::string_view token);

}  // namespace thelife::auth
