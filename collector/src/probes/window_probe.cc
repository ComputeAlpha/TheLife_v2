#include "probes/window_probe.h"

#include <unistd.h>

#include <cstdio>

#include "core/json.h"

namespace thelife::collector {

std::optional<std::string> readExe(const std::string& procRoot, int pid) {
  const auto link = procRoot + "/" + std::to_string(pid) + "/exe";
  char buf[4096];
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return std::nullopt;
  std::string exe(buf, static_cast<size_t>(n));
  if (exe.ends_with(" (deleted)")) exe.resize(exe.size() - 10);  // upgraded in place
  return exe;
}

Json::Value WindowProbe::payloadOf(const WindowInfo& w) const {
  Json::Value p;
  char id[16];
  std::snprintf(id, sizeof id, "0x%x", w.id);
  p["window_id"] = id;
  p["wm_class"].append(json::utf8Truncate(w.wmInstance, 256));
  p["wm_class"].append(json::utf8Truncate(w.wmClass, 256));
  p["title"] = json::utf8Truncate(w.title, 1024);
  p["pid"] = w.pid ? Json::Value(*w.pid) : Json::Value();
  const auto exe = w.pid ? readExe(procRoot_, *w.pid) : std::nullopt;
  p["exe"] = exe ? Json::Value(json::utf8Truncate(*exe, 4096)) : Json::Value();
  const auto mon = w.geometry ? monitorOf(monitors_, *w.geometry) : std::nullopt;
  p["monitor"] = mon ? Json::Value(*mon) : Json::Value();
  p["fullscreen"] = w.fullscreen;
  return p;
}

uint32_t WindowProbe::refresh(SysTime now, bool switched) {
  const uint32_t id = x_.activeWindow();
  const auto info = x_.window(id);
  pid_ = info.pid;
  current_ = payloadOf(info);
  if (switched) {
    pending_.reset();  // a title change of the window we just left no longer matters
    if (current_ != lastEmitted_) {
      emit_(now, current_);
      lastEmitted_ = current_;
    }
  } else if (current_ != lastEmitted_) {
    if (!pending_) pendingFirst_ = now;
    pending_ = current_;
    pendingAt_ = now;
  } else {
    pending_.reset();  // changed and changed back within the quiet period
  }
  return id;
}

void WindowProbe::tick(SysTime now) {
  if (!pending_) return;
  if (now - pendingAt_ < kQuiet && now - pendingFirst_ < kMaxDelay) return;
  emit_(pendingAt_, *pending_);
  lastEmitted_ = *pending_;
  pending_.reset();
}

std::optional<int> WindowProbe::monitor() const {
  if (current_["monitor"].isInt()) return current_["monitor"].asInt();
  return std::nullopt;
}

std::string WindowProbe::appKey() const {
  const auto& c = current_["wm_class"];
  return c.isArray() && c.size() == 2 ? c[1].asString() : "";
}

}  // namespace thelife::collector
