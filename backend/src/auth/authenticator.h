#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/utils/coroutine.h>

#include <optional>
#include <string_view>

#include "auth/principal.h"

namespace thelife::auth {

// The web UI's session cookie (frontend_lld.md §2.2).
inline constexpr const char* kSessionCookie = "tl_session";

// Resolves "Authorization: Bearer <token>" to a Principal through auth.authenticate() (SECURITY DEFINER). Without
// an Authorization header, the tl_session cookie is tried instead; it only ever yields a user session (a device
// token or API key in the cookie is refused), and *viaCookie is set so the caller can demand the CSRF header.
// Returns nullopt for missing, malformed, unknown, revoked or expired tokens. Malformed tokens never reach
// the database.
drogon::Task<std::optional<Principal>> authenticate(const drogon::HttpRequestPtr& req, bool* viaCookie = nullptr);
drogon::Task<std::optional<Principal>> authenticateToken(std::string_view token);

}  // namespace thelife::auth
