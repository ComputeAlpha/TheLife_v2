#include "config/server_config.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace thelife::collector {

namespace {

bool intField(const Json::Value& v, const char* key, int lo, int hi, int& out, ParsedServerConfig& p,
              std::string* err) {
  const auto& f = v[key];
  // Integers only; a JSON number with a fraction or out of int64 range is a type error.
  if (!f.isNumeric() || f.isBool() || (f.isDouble() && std::floor(f.asDouble()) != f.asDouble())) {
    if (err) *err = std::string(key) + " must be an integer";
    return false;
  }
  const double d = f.asDouble();
  const long long raw = d > 1e12 ? static_cast<long long>(1e12) : d < -1e12 ? static_cast<long long>(-1e12)
                                                                             : static_cast<long long>(d);
  const long long c = std::clamp<long long>(raw, lo, hi);
  if (c != raw) p.clamped.push_back(std::format("{} {} -> {}", key, raw, c));
  out = static_cast<int>(c);
  return true;
}

}  // namespace

std::optional<ParsedServerConfig> parseServerConfig(const Json::Value& v, std::string* err) {
  if (!v.isObject()) {
    if (err) *err = "config must be an object";
    return std::nullopt;
  }
  ParsedServerConfig p;
  auto& c = p.config;
  if (!v["enabled"].isBool()) {
    if (err) *err = "enabled must be a boolean";
    return std::nullopt;
  }
  c.enabled = v["enabled"].asBool();
  const auto& m = v["capture_monitors"];
  if (!m.isString() || (m.asString() != "all" && m.asString() != "active")) {
    if (err) *err = "capture_monitors must be all or active";
    return std::nullopt;
  }
  c.captureMonitors = m.asString();
  using B = Bounds;
  if (!intField(v, "capture_interval_s", B::kMinCaptureIntervalS, B::kMaxCaptureIntervalS, c.captureIntervalS, p,
                err) ||
      !intField(v, "jitter_pct", B::kMinJitterPct, B::kMaxJitterPct, c.jitterPct, p, err) ||
      !intField(v, "capture_max_px", B::kMinCaptureMaxPx, B::kMaxCaptureMaxPx, c.captureMaxPx, p, err) ||
      !intField(v, "idle_threshold_s", B::kMinIdleThresholdS, B::kMaxIdleThresholdS, c.idleThresholdS, p, err) ||
      !intField(v, "process_interval_s", B::kMinProcessIntervalS, B::kMaxProcessIntervalS, c.processIntervalS, p,
                err)) {
    return std::nullopt;
  }
  return p;
}

Json::Value toJson(const ServerConfig& c) {
  Json::Value v;
  v["enabled"] = c.enabled;
  v["capture_interval_s"] = c.captureIntervalS;
  v["jitter_pct"] = c.jitterPct;
  v["capture_monitors"] = c.captureMonitors;
  v["capture_max_px"] = c.captureMaxPx;
  v["idle_threshold_s"] = c.idleThresholdS;
  v["process_interval_s"] = c.processIntervalS;
  return v;
}

std::vector<std::string> describeChanges(const ServerConfig& a, const ServerConfig& b) {
  std::vector<std::string> out;
  const auto ja = toJson(a), jb = toJson(b);
  for (const auto& k : jb.getMemberNames()) {
    if (ja[k] != jb[k]) {
      auto s = [](const Json::Value& x) -> std::string {
        if (x.isString()) return x.asString();
        if (x.isBool()) return x.asBool() ? "true" : "false";
        return std::to_string(x.asInt());
      };
      out.push_back(std::format("{}: {} -> {}", k, s(ja[k]), s(jb[k])));
    }
  }
  return out;
}

}  // namespace thelife::collector
