#pragma once

namespace thelife::http {

// Web chat (agent_lld.md §7.1): POST /chat/web/messages, GET /chat/turns/{id}, GET /chat/web/stream (SSE).
void registerChatRoutes();

}  // namespace thelife::http
