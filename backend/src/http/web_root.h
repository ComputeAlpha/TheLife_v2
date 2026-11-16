#pragma once

#include <string>

namespace thelife::http {

// Serves the built web UI from `root` (THELIFE_WEB_ROOT, frontend_lld.md §2.1) on the API's origin, so the browser
// needs no CORS. GET and HEAD requests outside /api/, /healthz and /readyz get the file under `root`, or
// index.html when there is none (client-side routes). Paths never leave `root`: dot segments are refused and the
// resolved path must stay inside it. Hashed files under assets/ are cached as immutable; everything else is
// no-cache. Every response carries the CSP and the other security headers.
void installWebRoot(const std::string& root);

}  // namespace thelife::http
