// libFuzzer target: everything GET /devices/self/config can send (collector_lld.md §8).
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "config/server_config.h"
#include "core/json.h"

using namespace thelife::collector;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const auto doc = json::parseStrict(std::string_view(reinterpret_cast<const char*>(data), size));
  if (!doc) return 0;
  std::string err;
  if (const auto p = parseServerConfig(*doc, &err)) {
    const auto& c = p->config;
    // Whatever the server sent, the local bounds hold.
    if (c.captureIntervalS < Bounds::kMinCaptureIntervalS || c.captureMaxPx > Bounds::kMaxCaptureMaxPx ||
        c.processIntervalS < Bounds::kMinProcessIntervalS || c.idleThresholdS < Bounds::kMinIdleThresholdS ||
        c.idleThresholdS > Bounds::kMaxIdleThresholdS || c.jitterPct < 0 || c.jitterPct > Bounds::kMaxJitterPct ||
        (c.captureMonitors != "all" && c.captureMonitors != "active")) {
      __builtin_trap();
    }
    describeChanges(ServerConfig{}, c);
  }
  return 0;
}
