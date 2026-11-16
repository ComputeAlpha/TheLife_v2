#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>

#include "core/time.h"

namespace thelife::collector {

// Idle transitions (collector_lld.md §5.2), sampled every 5 s. `start` is backdated to the last input; a locked
// screen counts as idle at once; a fullscreen window without input is idle but "passive" (video, a lecture), and
// the backend decides what that means.
class IdleProbe {
 public:
  using Emit = std::function<void(SysTime at, const Json::Value& payload)>;
  explicit IdleProbe(Emit emit) : emit_(std::move(emit)) {}

  void sample(SysTime now, uint32_t msSinceInput, bool locked, bool fullscreenFocused, int thresholdS);

  bool idle() const { return idle_; }
  bool passive() const { return idle_ && passive_; }
  bool locked() const { return idle_ && locked_; }

  static constexpr auto kInterval = std::chrono::seconds(5);

 private:
  Emit emit_;
  bool idle_ = false, passive_ = false, locked_ = false;
};

}  // namespace thelife::collector
