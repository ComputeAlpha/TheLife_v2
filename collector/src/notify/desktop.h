#pragma once

#include <systemd/sd-bus.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "notify/notifications.h"


namespace thelife::collector {

// org.freedesktop.Notifications on the session bus (sd-bus). Not thread-safe: one thread shows notifications and
// processes the click signals.
class DesktopNotifier {
 public:
  static std::unique_ptr<DesktopNotifier> open(std::string* err);
  ~DesktopNotifier();

  // The server's notification id, or nullopt when the call failed.
  std::optional<uint32_t> show(const Note& n);
  // Dispatches bus messages for up to `timeoutMs`; `onClick(serverId)` runs for a clicked notification.
  void process(int timeoutMs, const std::function<void(uint32_t)>& onClick);

 private:
  DesktopNotifier() = default;
  static int onAction(sd_bus_message* m, void* ud, sd_bus_error* err);

  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_ = nullptr;
  std::function<void(uint32_t)> onClick_;
};

}  // namespace thelife::collector
