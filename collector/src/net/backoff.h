#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

namespace thelife::collector {

// Exponential backoff with full jitter, 2 s doubling to a 5 min cap (collector_lld.md §7.1). Never below 1 s.
class Backoff {
 public:
  explicit Backoff(uint64_t seed, std::chrono::seconds base = std::chrono::seconds(2),
                   std::chrono::seconds cap = std::chrono::seconds(300))
      : rng_(seed), base_(base), cap_(cap) {}

  std::chrono::seconds next() {
    const int64_t ceiling = std::min<int64_t>(cap_.count(), base_.count() << std::min(attempts_, 20));
    ++attempts_;
    std::uniform_int_distribution<int64_t> d(1, std::max<int64_t>(1, ceiling));
    return std::chrono::seconds(d(rng_));
  }
  void reset() { attempts_ = 0; }
  int attempts() const { return attempts_; }

 private:
  std::mt19937_64 rng_;
  std::chrono::seconds base_, cap_;
  int attempts_ = 0;
};

}  // namespace thelife::collector
