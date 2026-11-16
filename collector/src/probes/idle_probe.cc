#include "probes/idle_probe.h"

namespace thelife::collector {

void IdleProbe::sample(SysTime now, uint32_t ms, bool locked, bool fullscreen, int thresholdS) {
  const auto sinceInput = std::chrono::milliseconds(ms);
  const bool inputIdle = sinceInput >= std::chrono::seconds(thresholdS);
  auto event = [&](const char* kind, SysTime at, bool passive, bool lock) {
    Json::Value p;
    p["event"] = kind;
    p["at"] = rfc3339(at);
    if (std::string(kind) == "start") {
      p["passive"] = passive;
      p["locked"] = lock;
    }
    emit_(now, p);
  };
  if (!idle_) {
    if (locked || inputIdle) {
      idle_ = true;
      locked_ = locked;
      passive_ = !locked && fullscreen;
      // A lock is reported when seen; plain idleness from the last input.
      event("start", locked && !inputIdle ? now : now - sinceInput, passive_, locked_);
    }
    return;
  }
  if (!locked && !inputIdle) {
    idle_ = passive_ = locked_ = false;
    event("end", now - sinceInput, false, false);
    return;
  }
  if (locked && !locked_) {
    locked_ = true;
    passive_ = false;
    event("start", now, false, true);
  } else if (passive_ && !fullscreen) {
    passive_ = false;  // the video ended and nobody came back
    event("start", now, false, locked_);
  }
}

}  // namespace thelife::collector
