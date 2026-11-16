#pragma once

#include <json/json.h>
#include <sys/types.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/time.h"

namespace thelife::collector {

struct ProcEntry {
  std::string exe, comm;
  std::vector<int> pids;
  double cpuPct = 0, rssMb = 0;
  bool hasWindow = false, focused = false;
  std::vector<std::string> args;
  Json::Value toJson() const;
};

// Process snapshots (collector_lld.md §5.3): the session user's processes only, kernel threads dropped, grouped by
// executable. Arguments are sent only for executables in the local allowlist. A full snapshot every 10 min, deltas
// (started / exited / changed) in between. `procRoot` is /proc, or a fake tree in tests.
class ProcProbe {
 public:
  ProcProbe(std::string procRoot, uid_t uid, std::vector<std::string> argsAllowlist);

  // The payload to queue: {full:true, procs} or {full:false, started, changed, exited} ({full:false} alone when
  // nothing changed).
  Json::Value snapshot(SysTime now, std::optional<int> focusedPid, const std::vector<int>& windowPids);
  // The latest aggregation, busiest first (the capture context).
  std::vector<ProcEntry> top(size_t n) const;

  static constexpr auto kFullEvery = std::chrono::minutes(10);
  static constexpr size_t kMaxEntries = 500;

 private:
  struct Sample {
    std::string exe, comm;
    unsigned long long ticks = 0;
    double rssMb = 0;
    std::vector<std::string> args;
  };
  std::map<int, Sample> scan() const;
  static bool changed(const ProcEntry& a, const ProcEntry& b);

  std::string root_;
  uid_t uid_;
  std::set<std::string> allow_;
  long clkTck_;
  std::map<int, unsigned long long> lastTicks_;
  std::optional<SysTime> lastScan_, lastFull_;
  std::map<std::string, ProcEntry> current_, sent_;
};

}  // namespace thelife::collector
