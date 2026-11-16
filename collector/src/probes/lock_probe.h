#pragma once

#include <memory>
#include <string>

struct sd_bus;

namespace thelife::collector {

// Screen-lock state (collector_lld.md §5.2) beyond the X screensaver: logind's LockedHint for the user's graphical
// session (GNOME, KDE and light-locker set it), and a few lockers that show up as the focused window.
class LockProbe {
 public:
  LockProbe();
  ~LockProbe();
  bool locked(const std::string& focusedWmClass);

  static bool isLockerClass(const std::string& wmClass);

 private:
  bool logindLocked();
  sd_bus* bus_ = nullptr;
  std::string sessionPath_;
  bool unavailable_ = false;
};

}  // namespace thelife::collector
