#pragma once

namespace thelife::http {

// /api/v1/auth/login, /auth/logout, /me (GET, PATCH), /me/password: the tool:false endpoints of
// api/resources/auth.yaml.
void registerAuthRoutes();

}  // namespace thelife::http
