#pragma once

#include <json/json.h>

#include <optional>
#include <string>
#include <vector>

namespace thelife::collector {

// GET /devices/self/config (collector_lld.md §6). Every value is clamped to local bounds that no server value can
// exceed (§8); `enabled` starts false so nothing is captured before the first config arrives.
struct ServerConfig {
  bool enabled = false;
  int captureIntervalS = 300;
  int jitterPct = 10;
  std::string captureMonitors = "all";
  int captureMaxPx = 1600;
  int idleThresholdS = 120;
  int processIntervalS = 60;

  bool operator==(const ServerConfig&) const = default;
};

struct Bounds {
  static constexpr int kMinCaptureIntervalS = 60, kMaxCaptureIntervalS = 86400;
  static constexpr int kMinJitterPct = 0, kMaxJitterPct = 50;
  static constexpr int kMinCaptureMaxPx = 320, kMaxCaptureMaxPx = 2560;
  static constexpr int kMinIdleThresholdS = 30, kMaxIdleThresholdS = 1800;
  static constexpr int kMinProcessIntervalS = 15, kMaxProcessIntervalS = 3600;
};

struct ParsedServerConfig {
  ServerConfig config;
  std::vector<std::string> clamped;  // "capture_interval_s 5 -> 60"
};

// Rejects the document (nullopt) when a required field is missing or has the wrong type; ignores unknown fields.
std::optional<ParsedServerConfig> parseServerConfig(const Json::Value& v, std::string* err);
Json::Value toJson(const ServerConfig& c);
// "capture_interval_s: 300 -> 600" for every changed field (the local audit log).
std::vector<std::string> describeChanges(const ServerConfig& from, const ServerConfig& to);

}  // namespace thelife::collector
