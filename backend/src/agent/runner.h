#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>

#include "agent/store.h"
#include "auth/principal.h"

namespace thelife::agent {

// One unit of agent work (agent_lld.md §4.1). user_message turns exist as a `turns` row before they are
// queued; approval turns create theirs when they run.
struct TurnRequest {
  std::string tenantId, userId;
  std::string channel = "web";
  std::string trigger = "user_message";  // user_message | approval
  std::string turnId;                     // user_message
  std::string approvalId;                 // approval
};

// Queues the turn; turns of one (user, channel) run one at a time, in order (§4.5).
void submitTurn(TurnRequest r);

// Runs a turn to completion (exposed for tests; normally driven by submitTurn).
drogon::Task<void> runTurn(TurnRequest r);

// Limits per turn (§5).
inline constexpr int kMaxSteps = 8;
inline constexpr int kMaxToolCalls = 30;

// Deterministic command handling (§3.4): /help, /today, /tasks, /done N, /skip N, /approvals, /approve N,
// /reject N. Runs as the user, with no model. Returns the reply text.
drogon::Task<std::string> runCommand(const auth::Principal& user, const std::string& text, const NowInfo& now);

// Reply when no model can answer (manual mode or no LLM configured).
std::string offlineReply(const std::string& mode, const std::string& resetsLocal);

}  // namespace thelife::agent
