#pragma once

namespace thelife::http {

// GET /api/v1/system/llm-status: gateway mode, provider states and this week's usage (agent_lld.md §3.4–3.5).
void registerLlmRoutes();

}  // namespace thelife::http
