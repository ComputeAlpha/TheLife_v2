#include "auth/rate_limiter.h"

namespace thelife::auth {

using Clock = std::chrono::steady_clock;

void FailureLimiter::pruneLocked(Clock::time_point now) {
  if (now - lastPrune_ < window_) return;
  lastPrune_ = now;
  for (auto it = entries_.begin(); it != entries_.end();) {
    it = (now - it->second.windowStart >= window_) ? entries_.erase(it) : std::next(it);
  }
}

int FailureLimiter::blockedFor(const std::string& key) {
  std::lock_guard lock(mu_);
  const auto now = Clock::now();
  pruneLocked(now);
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.failures < max_) return 0;
  const auto left = window_ - (now - it->second.windowStart);
  if (left <= Clock::duration::zero()) {
    entries_.erase(it);
    return 0;
  }
  return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(left).count()) + 1;
}

void FailureLimiter::recordFailure(const std::string& key) {
  std::lock_guard lock(mu_);
  const auto now = Clock::now();
  auto& e = entries_[key];
  if (e.failures == 0 || now - e.windowStart >= window_) {
    e.failures = 0;
    e.windowStart = now;
  }
  ++e.failures;
}

void FailureLimiter::reset(const std::string& key) {
  std::lock_guard lock(mu_);
  entries_.erase(key);
}

}  // namespace thelife::auth
