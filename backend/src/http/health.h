#pragma once

namespace thelife::http {

// GET /healthz  liveness: process is up and serving.
// GET /readyz   readiness: database reachable and at the expected schema version.
void registerHealthRoutes();

}  // namespace thelife::http
