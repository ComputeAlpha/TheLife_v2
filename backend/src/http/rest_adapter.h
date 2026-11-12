#pragma once

namespace thelife::http {

// Registers a route for every bound tool that has a REST mapping (api/generated/tools.json `rest`): collects
// path, query and body arguments into one object, enforces If-Match and Idempotency-Key, invokes the tool
// registry, and maps the outcome to the HTTP response (docs/tool_registry.md §5).
void registerToolRoutes();

}  // namespace thelife::http
