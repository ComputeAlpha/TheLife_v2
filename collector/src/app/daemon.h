#pragma once

#include <string>

#include "config/local_config.h"
#include "config/token_store.h"
#include "core/time.h"

namespace thelife::collector {

// Timers of the daemon. THELIFE_COLLECTOR_FAST_TIMERS=1 shortens them for the tests; it changes only how soon
// things happen locally, never the server-side bounds of §8 (capture interval >= 60 s etc.).
struct Timing {
  Seconds firstCapture{60}, configEvery{300}, heartbeatEvery{60}, uploadDelay{15}, stateEvery{15};
  Seconds rotateCheckEvery{3600}, tokenMaxAge{30 * 86400}, capCheckEvery{60}, focusSettle{30};
  Seconds tokenReloadEvery{60}, backoffCap{300};
  static Timing fromEnv();
};

struct DaemonOptions {
  std::string display;  // empty: $DISPLAY
  std::string procRoot = "/proc";
  bool useLogind = true;
  Timing timing;
};

// Exit codes (systemd restarts on failure, collector_lld.md §4).
constexpr int kExitOk = 0;
constexpr int kExitConfig = 2;
constexpr int kExitXLost = 3;

int runDaemon(const LocalConfig& cfg, TokenStore& token, const DaemonOptions& opt);

}  // namespace thelife::collector
