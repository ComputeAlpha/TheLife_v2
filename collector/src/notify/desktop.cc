#include "notify/desktop.h"

#include <systemd/sd-bus.h>

#include <cstring>

#include "core/log.h"

namespace thelife::collector {

std::unique_ptr<DesktopNotifier> DesktopNotifier::open(std::string* err) {
  std::unique_ptr<DesktopNotifier> d(new DesktopNotifier());
  int r = sd_bus_open_user(&d->bus_);
  if (r < 0) {
    if (err) *err = std::string("session bus: ") + std::strerror(-r);
    return nullptr;
  }
  r = sd_bus_match_signal(d->bus_, &d->slot_, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                          "org.freedesktop.Notifications", "ActionInvoked", &DesktopNotifier::onAction, d.get());
  if (r < 0) {
    if (err) *err = std::string("session bus match: ") + std::strerror(-r);
    return nullptr;
  }
  return d;
}

DesktopNotifier::~DesktopNotifier() {
  sd_bus_slot_unref(slot_);
  sd_bus_flush_close_unref(bus_);
}

int DesktopNotifier::onAction(sd_bus_message* m, void* ud, sd_bus_error*) {
  auto* self = static_cast<DesktopNotifier*>(ud);
  uint32_t id = 0;
  const char* key = nullptr;
  if (sd_bus_message_read(m, "us", &id, &key) >= 0 && key && std::strcmp(key, "default") == 0 && self->onClick_) {
    self->onClick_(id);
  }
  return 0;
}

std::optional<uint32_t> DesktopNotifier::show(const Note& n) {
  sd_bus_message* msg = nullptr;
  sd_bus_message* reply = nullptr;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  std::optional<uint32_t> out;
  const std::string title = plainText(n.title, 80), body = plainText(n.body, 400);
  const uint8_t urgency = n.urgency == "low" ? 0 : n.urgency == "critical" ? 2 : 1;
  int r = sd_bus_message_new_method_call(bus_, &msg, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                         "org.freedesktop.Notifications", "Notify");
  if (r >= 0) r = sd_bus_message_append(msg, "susss", "TheLife", 0u, "", title.c_str(), body.c_str());
  if (r >= 0) {
    if (!n.action.empty()) r = sd_bus_message_append(msg, "as", 2, "default", "Open");
    else r = sd_bus_message_append(msg, "as", 0);
  }
  if (r >= 0) r = sd_bus_message_append(msg, "a{sv}", 1, "urgency", "y", urgency);
  if (r >= 0) r = sd_bus_message_append(msg, "i", -1);
  if (r >= 0) r = sd_bus_call(bus_, msg, 5'000'000, &error, &reply);
  if (r >= 0) {
    uint32_t id = 0;
    if (sd_bus_message_read(reply, "u", &id) >= 0) out = id;
  } else {
    log::warn("notify: Notify failed: {}", error.message ? error.message : std::strerror(-r));
  }
  sd_bus_error_free(&error);
  sd_bus_message_unref(reply);
  sd_bus_message_unref(msg);
  return out;
}

void DesktopNotifier::process(int timeoutMs, const std::function<void(uint32_t)>& onClick) {
  onClick_ = onClick;
  for (;;) {
    const int r = sd_bus_process(bus_, nullptr);
    if (r < 0) break;
    if (r > 0) continue;
    sd_bus_wait(bus_, static_cast<uint64_t>(timeoutMs) * 1000);
    while (sd_bus_process(bus_, nullptr) > 0) {
    }
    break;
  }
  onClick_ = nullptr;
}

}  // namespace thelife::collector
