#include "notify/notifications.h"

#include "core/json.h"

namespace thelife::collector {

bool isUuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

std::vector<Note> parseNotes(const Json::Value& doc, int* skipped) {
  std::vector<Note> out;
  int bad = 0;
  if (doc.isObject() && doc["items"].isArray()) {
    for (const auto& it : doc["items"]) {
      if (out.size() >= 20) break;
      if (!it.isObject() || !it["id"].isString() || !isUuid(it["id"].asString()) || !it["title"].isString() ||
          !it["body"].isString()) {
        ++bad;
        continue;
      }
      Note n;
      n.id = it["id"].asString();
      n.title = it["title"].asString();
      n.body = it["body"].asString();
      const auto u = it["urgency"].isString() ? it["urgency"].asString() : "";
      n.urgency = u == "low" || u == "critical" ? u : "normal";
      const auto a = it["action"].isString() ? it["action"].asString() : "";
      if (a == "open_chat" || a == "open_approval") n.action = a;
      if (it["ref_id"].isString() && isUuid(it["ref_id"].asString())) n.refId = it["ref_id"].asString();
      if (n.action == "open_approval" && n.refId.empty()) n.action = "open_chat";
      out.push_back(std::move(n));
    }
  }
  if (skipped) *skipped = bad;
  return out;
}

std::string plainText(const std::string& s, size_t maxChars) {
  const std::string clean = json::utf8Truncate(s, s.size());  // valid UTF-8, no control characters
  std::string out;
  size_t chars = 0;
  for (size_t i = 0; i < clean.size() && chars < maxChars;) {
    const auto c = static_cast<unsigned char>(clean[i]);
    const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    switch (clean[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.append(clean, i, len);
    }
    i += len;
    ++chars;
  }
  return out;
}

std::optional<std::string> clickUrl(const std::string& webUiUrl, const Note& n) {
  if (webUiUrl.empty() || n.action.empty()) return std::nullopt;
  // The demo UI shows the chat and its pending approvals on one page; the ref id is only a UUID or nothing.
  if (n.action == "open_approval" && isUuid(n.refId)) return webUiUrl + "/chat#approval-" + n.refId;
  return webUiUrl + "/chat";
}

bool NoteHolder::add(const Note& n, SysTime now) {
  if (seen_.count(n.id)) return false;
  seen_.insert(n.id);
  seenOrder_.push_back(n.id);
  while (seenOrder_.size() > 500) {
    seen_.erase(seenOrder_.front());
    seenOrder_.pop_front();
  }
  held_.emplace_back(n, now);
  return true;
}

std::vector<Note> NoteHolder::due(SysTime now, bool userPresent) {
  while (!held_.empty() && now - held_.front().second > kHoldFor) held_.pop_front();
  std::vector<Note> out;
  if (!userPresent) return out;
  for (auto& [n, _] : held_) out.push_back(std::move(n));
  held_.clear();
  return out;
}

}  // namespace thelife::collector
