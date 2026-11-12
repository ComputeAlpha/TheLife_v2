#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace thelife::auth {

// Fixed-window failure counter (in-process; fine while the backend is a single process, backend_lld.md §9).
// blockedFor() > 0 means the key is locked out for that many seconds.
class FailureLimiter {
 public:
  FailureLimiter(int maxFailures, std::chrono::seconds window) : max_(maxFailures), window_(window) {}

  int blockedFor(const std::string& key);
  void recordFailure(const std::string& key);
  void reset(const std::string& key);

 private:
  struct Entry {
    int failures = 0;
    std::chrono::steady_clock::time_point windowStart;
  };
  void pruneLocked(std::chrono::steady_clock::time_point now);

  int max_;
  std::chrono::seconds window_;
  std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
  std::chrono::steady_clock::time_point lastPrune_{};
};

}  // namespace thelife::auth
