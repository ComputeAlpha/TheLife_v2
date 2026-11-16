#include <gtest/gtest.h>

#include "config/local_config.h"
#include "config/server_config.h"
#include "core/json.h"

using namespace thelife::collector;

TEST(LocalConfig, HttpsAnywhereButPlainHttpOnlyOnLoopback) {
  std::string err;
  EXPECT_TRUE(validBackendUrl("https://life.example.org", &err));
  EXPECT_TRUE(validBackendUrl("https://life.example.org:8443/prefix", &err));
  EXPECT_TRUE(validBackendUrl("http://127.0.0.1:8080", &err));
  EXPECT_TRUE(validBackendUrl("http://localhost:8080", &err));
  EXPECT_TRUE(validBackendUrl("http://[::1]:8080", &err));
  EXPECT_FALSE(validBackendUrl("http://192.168.1.4:8080", &err));
  EXPECT_FALSE(validBackendUrl("http://localhost.evil.com", &err));
  EXPECT_FALSE(validBackendUrl("ftp://127.0.0.1", &err));
  EXPECT_FALSE(validBackendUrl("https://user:pw@life.example.org", &err));
  EXPECT_FALSE(validBackendUrl("https://life.example.org/?x=1", &err));
  EXPECT_FALSE(validBackendUrl("life.example.org", &err));
}

TEST(LocalConfig, ParsesAndRendersTheSameValues) {
  std::string err;
  const auto c = parseLocalConfig(R"(
backend_url = "https://life.example.org/"
web_ui_url = "http://127.0.0.1:5000"
log_level = "debug"
args_allowlist = ["code", "idea.sh"]
pin_sha256 = "sha256//AAAA"
)",
                                  &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->backendUrl, "https://life.example.org");
  EXPECT_EQ(c->webUiUrl, "http://127.0.0.1:5000");
  EXPECT_EQ(c->argsAllowlist, (std::vector<std::string>{"code", "idea.sh"}));
  const auto again = parseLocalConfig(renderLocalConfig(*c), &err);
  ASSERT_TRUE(again) << err;
  EXPECT_EQ(again->backendUrl, c->backendUrl);
  EXPECT_EQ(again->argsAllowlist, c->argsAllowlist);
  EXPECT_EQ(again->pinSha256, c->pinSha256);
}

TEST(LocalConfig, RejectsBadValues) {
  std::string err;
  EXPECT_FALSE(parseLocalConfig("backend_url = \"http://10.0.0.2\"", &err));
  EXPECT_FALSE(parseLocalConfig("backend_url = \"https://a.b\"\nlog_level = \"loud\"", &err));
  EXPECT_FALSE(parseLocalConfig("backend_url = \"https://a.b\"\nargs_allowlist = [\"/usr/bin/code\"]", &err));
  EXPECT_FALSE(parseLocalConfig("backend_url = \"https://a.b\"\npin_sha256 = \"abc\"", &err));
  EXPECT_FALSE(parseLocalConfig("backend_url = \"https://a.b\"\nweb_ui_url = \"javascript:alert(1)\"", &err));
  EXPECT_FALSE(parseLocalConfig("backend_url = ", &err));
}

namespace {
Json::Value cfg(const char* text) { return *json::parseStrict(text); }
}  // namespace

TEST(ServerConfig, AcceptsTheBackendShape) {
  std::string err;
  const auto p = parseServerConfig(cfg(R"({"enabled": true, "capture_interval_s": 300, "jitter_pct": 10,
    "capture_monitors": "all", "capture_max_px": 1600, "idle_threshold_s": 120, "process_interval_s": 60,
    "something_new": {"ignored": true}})"),
                                   &err);
  ASSERT_TRUE(p) << err;
  EXPECT_TRUE(p->config.enabled);
  EXPECT_EQ(p->config.captureIntervalS, 300);
  EXPECT_TRUE(p->clamped.empty());
}

TEST(ServerConfig, ClampsEveryValueToLocalBounds) {
  std::string err;
  const auto p = parseServerConfig(cfg(R"({"enabled": true, "capture_interval_s": 1, "jitter_pct": 99,
    "capture_monitors": "active", "capture_max_px": 99999, "idle_threshold_s": 5, "process_interval_s": 1})"),
                                   &err);
  ASSERT_TRUE(p) << err;
  EXPECT_EQ(p->config.captureIntervalS, 60);
  EXPECT_EQ(p->config.jitterPct, 50);
  EXPECT_EQ(p->config.captureMaxPx, 2560);
  EXPECT_EQ(p->config.idleThresholdS, 30);
  EXPECT_EQ(p->config.processIntervalS, 15);
  EXPECT_EQ(p->clamped.size(), 5u);
  const auto huge = parseServerConfig(cfg(R"({"enabled": true, "capture_interval_s": 1e300, "jitter_pct": 0,
    "capture_monitors": "all", "capture_max_px": -5, "idle_threshold_s": 1800, "process_interval_s": 3600})"),
                                      &err);
  ASSERT_TRUE(huge) << err;
  EXPECT_EQ(huge->config.captureIntervalS, 86400);
  EXPECT_EQ(huge->config.captureMaxPx, 320);
}

TEST(ServerConfig, RejectsMissingOrMistypedFields) {
  std::string err;
  const char* base = R"({"enabled": true, "capture_interval_s": 300, "jitter_pct": 10, "capture_monitors": "all",
    "capture_max_px": 1600, "idle_threshold_s": 120, "process_interval_s": 60})";
  auto with = [&](const char* key, const Json::Value& v) {
    auto j = cfg(base);
    if (v.isNull()) j.removeMember(key);
    else j[key] = v;
    return parseServerConfig(j, &err);
  };
  EXPECT_FALSE(with("enabled", Json::Value()));
  EXPECT_FALSE(with("enabled", "true"));
  EXPECT_FALSE(with("capture_interval_s", "300"));
  EXPECT_FALSE(with("capture_interval_s", 300.5));
  EXPECT_FALSE(with("capture_monitors", "everything"));
  EXPECT_FALSE(with("capture_monitors", Json::Value()));
  EXPECT_FALSE(parseServerConfig(Json::Value(Json::arrayValue), &err));
  // Fields that look like commands or URLs are simply not part of the protocol: ignored.
  auto j = cfg(base);
  j["exec"] = "rm -rf ~";
  j["upload_url"] = "https://evil.example/";
  j["capture_now"] = true;
  const auto p = parseServerConfig(j, &err);
  ASSERT_TRUE(p);
  EXPECT_EQ(toJson(p->config).getMemberNames().size(), 7u);
}

TEST(ServerConfig, DescribesChangesForTheAuditLog) {
  ServerConfig a, b;
  b.enabled = true;
  b.captureIntervalS = 600;
  const auto d = describeChanges(a, b);
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0], "capture_interval_s: 300 -> 600");
  EXPECT_EQ(d[1], "enabled: false -> true");
}
