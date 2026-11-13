#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <optional>
#include <string>

#include "db/tenant_tx.h"

// Notifier (agent_lld.md §4.3, §8): the "less bothering" gate for proactive messages and delivery routing.
namespace thelife::proactive {

// Defaults (decided 2026-11-11): quiet hours 22:30–07:30, at most 6 proactive messages a day, 45 min apart.
inline constexpr int kDailyCap = 6;
inline constexpr int kSpacingMinutes = 45;
inline constexpr int kTelegramFallbackSeconds = 180;

struct Gate {
  bool deliver = true;
  std::string reason;  // why it was held: quiet_hours | daily_cap | spacing
};

// Quiet hours hold everything, unless the user was active in the last 10 minutes (they are clearly awake: a
// morning plan at 06:40 answers someone who just started). Urgent skips the spacing, not quiet hours or the cap.
drogon::Task<Gate> gate(db::TenantTx& tx, const std::string& userId, bool urgent);

struct Notice {
  std::string userId, kind, urgency = "normal", title, body, action = "open_chat";
  std::optional<std::string> refType, refId, conversationId, messageId;
  bool proactive = false;
  bool telegramSent = false;  // the turn already sent it to the user's Telegram chat (a telegram-channel turn)
};

// Stores the notification and routes it: browser tabs holding the SSE stream get it now; the collector picks it
// up by long poll (desktop). With Telegram linked, a `notify.telegram` job sends it there: at once when the user
// is away (no web tab, no fresh collector heartbeat), else after 3 minutes unless acked by then. Returns the id.
drogon::Task<std::string> notify(db::TenantTx& tx, const Notice& n);

// Outbox (§4.2): a domain change that may start an agent turn, written in the caller's transaction.
drogon::Task<void> emit(db::TenantTx& tx, std::optional<std::string> userId, const std::string& kind,
                        const std::string& entityType, std::optional<std::string> entityId,
                        Json::Value payload = Json::Value(Json::objectValue));

}  // namespace thelife::proactive
