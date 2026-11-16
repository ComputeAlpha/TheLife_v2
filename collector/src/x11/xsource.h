#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/bytes.h"

namespace thelife::collector {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

struct Monitor {
  int index = 0;
  Rect r;
  bool primary = false;
};

struct WindowInfo {
  uint32_t id = 0;
  std::string wmInstance, wmClass, title;
  std::optional<int> pid;
  bool fullscreen = false;
  std::optional<Rect> geometry;  // root coordinates
};

struct IdleInfo {
  uint32_t msSinceInput = 0;
  bool screensaverOn = false;
};

// 32-bit BGRX pixels (little-endian 0x00RRGGBB), zeroed when released.
struct Image {
  int width = 0, height = 0, stride = 0;
  SecureBytes px;
};

// What the probes and capture need from the X server (collector_lld.md §9: IXSource). The real implementation is
// XcbSource; unit tests use a fake.
class IXSource {
 public:
  virtual ~IXSource() = default;
  virtual uint32_t activeWindow() = 0;  // 0 when none
  virtual WindowInfo window(uint32_t id) = 0;
  virtual IdleInfo idle() = 0;
  virtual std::vector<Monitor> monitors() = 0;
  virtual std::vector<int> clientPids() = 0;  // _NET_WM_PID of every managed window
  virtual std::optional<Image> grab(const Rect& r, std::string* err) = 0;
};

// Index of the monitor containing the centre of `r` (nullopt when none does).
std::optional<int> monitorOf(const std::vector<Monitor>& ms, const Rect& r);

}  // namespace thelife::collector
