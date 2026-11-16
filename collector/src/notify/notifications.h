#pragma once

#include <json/json.h>

#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/time.h"

namespace thelife::collector {

// A desktop notification from GET /devices/self/notifications (collector_lld.md §7.5). Data only: the backend
// never sends a URL, a command or markup that is interpreted.
struct Note {
  std::string id, title, body;
  std::string urgency = "normal";  // low | normal | critical
  std::string action;              // "", open_chat, open_approval
  std::string refId;               // UUID or ""
};

bool isUuid(const std::string& s);
// Items that fail validation are skipped; `skipped` counts them.
std::vector<Note> parseNotes(const Json::Value& doc, int* skipped = nullptr);
// Plain text for the notification server: caps the length in characters (title 80, body 400) and escapes
// & < > ' " so a server that renders body markup shows them literally.
std::string plainText(const std::string& s, size_t maxChars);
// <web_ui_url>/chat, built locally; nullopt when no click action applies.
std::optional<std::string> clickUrl(const std::string& webUiUrl, const Note& n);

// Notifications wait here while the user is idle or the screen is locked, for up to 10 minutes; then they are
// dropped (unacknowledged, so the backend falls back to Telegram). Ids already shown are not shown again.
class NoteHolder {
 public:
  static constexpr auto kHoldFor = std::chrono::minutes(10);
  // false when the note was already held or shown
  bool add(const Note& n, SysTime now);
  // Notes to show now (the user is present), oldest first; expired ones are dropped.
  std::vector<Note> due(SysTime now, bool userPresent);
  size_t held() const { return held_.size(); }

 private:
  std::deque<std::pair<Note, SysTime>> held_;
  std::set<std::string> seen_;
  std::deque<std::string> seenOrder_;
};

}  // namespace thelife::collector
