#include "probes/lock_probe.h"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace thelife::collector {

LockProbe::LockProbe() = default;
LockProbe::~LockProbe() { sd_bus_flush_close_unref(bus_); }

bool LockProbe::isLockerClass(const std::string& wmClass) {
  static constexpr std::array<const char*, 9> kLockers = {"i3lock",     "xsecurelock", "light-locker",
                                                          "xscreensaver", "slock",      "xlock",
                                                          "xtrlock",    "betterlockscreen", "gnome-screensaver"};
  std::string c = wmClass;
  std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return std::any_of(kLockers.begin(), kLockers.end(), [&](const char* l) { return c == l; });
}

bool LockProbe::logindLocked() {
  if (unavailable_) return false;
  if (!bus_) {
    if (sd_bus_open_system(&bus_) < 0) {
      unavailable_ = true;
      return false;
    }
  }
  if (sessionPath_.empty()) {
    // A systemd user service is not inside a session, so "session/auto" does not work; the user's Display
    // property names the graphical session.
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_get_property(bus_, "org.freedesktop.login1", "/org/freedesktop/login1/user/self",
                                      "org.freedesktop.login1.User", "Display", &e, &reply, "(so)");
    const char* id = nullptr;
    const char* path = nullptr;
    if (r >= 0 && sd_bus_message_read(reply, "(so)", &id, &path) >= 0 && path && *path && std::string(path) != "/") {
      sessionPath_ = path;
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&e);
    if (sessionPath_.empty()) {
      unavailable_ = true;
      return false;
    }
  }
  sd_bus_error e = SD_BUS_ERROR_NULL;
  int hint = 0;
  const int r = sd_bus_get_property_trivial(bus_, "org.freedesktop.login1", sessionPath_.c_str(),
                                            "org.freedesktop.login1.Session", "LockedHint", &e, 'b', &hint);
  sd_bus_error_free(&e);
  return r >= 0 && hint;
}

bool LockProbe::locked(const std::string& focusedWmClass) {
  return isLockerClass(focusedWmClass) || logindLocked();
}

}  // namespace thelife::collector
