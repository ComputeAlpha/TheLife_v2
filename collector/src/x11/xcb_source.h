#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "x11/xsource.h"

struct xcb_connection_t;

namespace thelife::collector {

// The X11 connection (collector_lld.md §5): EWMH properties for the active window, MIT-SCREEN-SAVER for idle time,
// RandR monitors, MIT-SHM (or GetImage) for captures, X-Resource as the PID fallback.
class XcbSource : public IXSource {
 public:
  static std::unique_ptr<XcbSource> connect(const char* display, std::string* err);
  ~XcbSource() override;

  uint32_t activeWindow() override;
  WindowInfo window(uint32_t id) override;
  IdleInfo idle() override;
  std::vector<Monitor> monitors() override;
  std::vector<int> clientPids() override;
  std::optional<Image> grab(const Rect& r, std::string* err) override;

  // Event integration for the main loop: the connection fd to poll(), and the changes seen since the last call.
  int fd() const;
  struct Events {
    bool activeChanged = false;
    bool windowChanged = false;  // title or state of the watched window
    bool lost = false;           // the X connection is gone (exit code 3)
  };
  Events drain();
  // Follow property changes of this window (the focused one); stops following the previous one.
  void watch(uint32_t id);
  bool ok() const;
  std::string screenName() const { return display_; }

 private:
  XcbSource() = default;
  uint32_t atom(const char* name);
  std::string property(uint32_t win, uint32_t atom, uint32_t type, uint32_t* actualType = nullptr);

  xcb_connection_t* c_ = nullptr;
  uint32_t root_ = 0;
  int rootDepth_ = 0;
  bool haveShm_ = false, haveSaver_ = false, haveRandr_ = false, haveRes_ = false;
  uint32_t watched_ = 0;
  std::string display_;
  std::unordered_map<std::string, uint32_t> atoms_;
};

}  // namespace thelife::collector
