#pragma once

#include <json/json.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/time.h"
#include "x11/xsource.h"

namespace thelife::collector {

// Focused-window observations (collector_lld.md §5.1). A switch of _NET_ACTIVE_WINDOW is reported at once; title
// and state changes of the same window are debounced (2 s quiet, at most 10 s) so animated titles produce one row.
class WindowProbe {
 public:
  using Emit = std::function<void(SysTime at, const Json::Value& payload)>;
  WindowProbe(IXSource& x, std::string procRoot, Emit emit) : x_(x), procRoot_(std::move(procRoot)), emit_(std::move(emit)) {}

  // Re-reads the focused window. `switched`: the active window changed. Returns the active window id.
  uint32_t refresh(SysTime now, bool switched);
  void tick(SysTime now);
  // Forget what was reported, so the next refresh reports the window again (after a pause).
  void forget() { lastEmitted_ = Json::Value(); pending_.reset(); }
  void setMonitors(std::vector<Monitor> ms) { monitors_ = std::move(ms); }

  // The current focused-window state (the last one read, emitted or not).
  const Json::Value& current() const { return current_; }
  std::optional<int> pid() const { return pid_; }
  bool fullscreen() const { return current_["fullscreen"].asBool(); }
  std::optional<int> monitor() const;
  std::string appKey() const;  // WM_CLASS class, for the context-change trigger

  static constexpr auto kQuiet = std::chrono::seconds(2);
  static constexpr auto kMaxDelay = std::chrono::seconds(10);

 private:
  Json::Value payloadOf(const WindowInfo& w) const;

  IXSource& x_;
  std::string procRoot_;
  Emit emit_;
  std::vector<Monitor> monitors_;
  Json::Value current_, lastEmitted_;
  std::optional<int> pid_;
  std::optional<Json::Value> pending_;
  SysTime pendingAt_{}, pendingFirst_{};
};

std::optional<std::string> readExe(const std::string& procRoot, int pid);

}  // namespace thelife::collector
