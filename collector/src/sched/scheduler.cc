#include "sched/scheduler.h"

#include <algorithm>

namespace thelife::collector {

Seconds Scheduler::jittered(int intervalS, int jitterPct) {
  const double j = std::clamp(jitterPct, 0, 50) / 100.0;
  std::uniform_real_distribution<double> d(1.0 - j, 1.0 + j);
  return Seconds(std::max<long>(1, static_cast<long>(intervalS * d(rng_))));
}

SchedDecision Scheduler::tick(SysTime now, const SchedState& s) {
  if (!nextDue_) nextDue_ = now + firstDelay_;
  if (now < *nextDue_) return {};
  nextDue_ = now + jittered(s.intervalS, s.jitterPct);
  auto skip = [](const char* reason) { return SchedDecision{SchedDecision::Kind::Skip, reason}; };
  if (s.paused) return skip("paused");
  if (!s.configKnown || !s.enabled) return skip("disabled");
  if (s.locked) return skip("locked");
  if (s.idle && !s.passive) return skip("idle");
  if (!s.online) return skip("offline");
  lastCapture_ = now;
  return {SchedDecision::Kind::Capture, ""};
}

bool Scheduler::pullForward(SysTime now, int intervalS) {
  if (lastCapture_ && now - *lastCapture_ < Seconds(intervalS)) return false;
  if (nextDue_ && *nextDue_ <= now) return false;  // already due
  nextDue_ = now;
  return true;
}

}  // namespace thelife::collector
