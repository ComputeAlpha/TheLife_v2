#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>

#include "core/time.h"

namespace thelife::collector {

// Capture scheduling (collector_lld.md §6): one capture per `interval` ± `jitter`, never on demand from the
// server. A due capture is skipped, with a reason, when the user is idle (not passive), the screen is locked, the
// local pause is on, the server disabled capture, or the backend is unreachable.
//
// The context-change trigger may pull a capture forward, but only once a full interval has passed since the last
// one, so the rate never exceeds the budget (a token bucket of size one).
struct SchedState {
  bool configKnown = false, enabled = false;
  bool idle = false, passive = false, locked = false;
  bool paused = false, online = false;
  int intervalS = 300, jitterPct = 10;
};

struct SchedDecision {
  enum class Kind { Wait, Capture, Skip } kind = Kind::Wait;
  std::string skipReason;  // capture_meta.skipped
};

class Scheduler {
 public:
  explicit Scheduler(uint64_t seed, Seconds firstDelay = Seconds(60)) : rng_(seed), firstDelay_(firstDelay) {}

  SchedDecision tick(SysTime now, const SchedState& s);
  // The focused app changed and stayed: capture now if a full interval has passed since the last capture.
  bool pullForward(SysTime now, int intervalS);
  std::optional<SysTime> nextDue() const { return nextDue_; }
  std::optional<SysTime> lastCapture() const { return lastCapture_; }


 private:
  Seconds jittered(int intervalS, int jitterPct);

  std::mt19937_64 rng_;
  Seconds firstDelay_;
  std::optional<SysTime> nextDue_, lastCapture_;
};

}  // namespace thelife::collector
