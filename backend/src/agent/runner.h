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
  std::string trigger = "user_message";  // user_message | approval | proactive | deferred_reply
  std::string turnId;                     // user_message, deferred_reply (an existing turns row)
  std::string approvalId;                 // approval
  // proactive (§4.1): what started it, with details for its preamble (prompts/agent/triggers/<kind>.md)
  std::string triggerKind;                // routine.morning | followup | tasks_missed | dispute_review | ...
  std::string details;
  bool silent = false;                    // never message the user (dispute review)
  bool urgent = false;
};

// Queues the turn; turns of one (user, channel) run one at a time, in order (§4.5).
void submitTurn(TurnRequest r);

// Runs a turn to completion (exposed for tests; normally driven by submitTurn).
drogon::Task<void> runTurn(TurnRequest r);

// Limits per turn (§5).
inline constexpr int kMaxSteps = 8;
inline constexpr int kMaxToolCalls = 30;
inline constexpr int kMaxWebCallsPerTool = 3;  // web.search and web.fetch each

// Deterministic command handling (§3.4): /help, /today, /tasks, /done N, /skip N, /approvals, /approve N,
// /reject N. Runs as the user, with no model. Returns the reply text.
drogon::Task<std::string> runCommand(const auth::Principal& user, const std::string& text, const NowInfo& now);

// Reply when no model can answer (manual mode or no LLM configured).
std::string offlineReply(const std::string& mode, const std::string& resetsLocal);

}  // namespace thelife::agent
