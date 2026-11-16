#include <gtest/gtest.h>
#include <sys/stat.h>

#include <fstream>

#include "config/token_store.h"
#include "core/files.h"
#include "core/json.h"
#include "core/time.h"
#include "support/fakes.h"

using namespace thelife::collector;
using thelife::collector::testing::TempDir;

TEST(Time, Rfc3339RoundTripsWithMilliseconds) {
  const auto t = SysTime(std::chrono::milliseconds(1763279160242));
  EXPECT_EQ(rfc3339(t), "2025-11-16T07:46:00.242Z");
  EXPECT_EQ(parseRfc3339("2025-11-16T07:46:00.242Z"), t);
  EXPECT_EQ(parseRfc3339("2025-11-16T09:46:00.242+02:00"), t);
  EXPECT_FALSE(parseRfc3339("2025-11-16 07:46:00Z"));
  EXPECT_FALSE(parseRfc3339("2025-11-16T07:46:00"));
  EXPECT_FALSE(parseRfc3339("2025-13-16T07:46:00Z"));
}

TEST(Time, DurationsAreBoundedAndStrict) {
  EXPECT_EQ(parseDuration("90s"), Seconds(90));
  EXPECT_EQ(parseDuration("1h30m"), Seconds(5400));
  EXPECT_EQ(parseDuration("1d"), Seconds(86400));
  EXPECT_FALSE(parseDuration(""));
  EXPECT_FALSE(parseDuration("0m"));
  EXPECT_FALSE(parseDuration("15"));
  EXPECT_FALSE(parseDuration("2w"));
  EXPECT_FALSE(parseDuration("31d"));
  EXPECT_EQ(formatDuration(Seconds(5400)), "1h30m");
}

TEST(Json, StrictParsingRejectsWhatABackendShouldNeverSend) {
  EXPECT_TRUE(json::parseStrict(R"({"a": [1, 2, {"b": null}]})"));
  EXPECT_FALSE(json::parseStrict(R"({"a": 1, "a": 2})"));  // duplicate keys
  EXPECT_FALSE(json::parseStrict(R"({"a": 1} trailing)"));
  EXPECT_FALSE(json::parseStrict("// comment\n{}"));
  std::string deep;
  for (int i = 0; i < 40; ++i) deep += "[";
  for (int i = 0; i < 40; ++i) deep += "]";
  EXPECT_FALSE(json::parseStrict(deep));  // nesting beyond 16
  std::string big = "\"" + std::string(json::kMaxResponseBytes, 'x') + "\"";
  EXPECT_FALSE(json::parseStrict(big));
}

TEST(Json, Utf8TruncateNeverSplitsACharacterOrKeepsControls) {
  EXPECT_EQ(json::utf8Truncate("héllo", 2), "h");  // é is two bytes
  EXPECT_EQ(json::utf8Truncate("héllo", 3), "hé");
  EXPECT_EQ(json::utf8Truncate("a\x01" "b", 10), "a b");
  EXPECT_EQ(json::utf8Truncate("a\xff" "b", 10), "a\xEF\xBF\xBD" "b");
}

TEST(Files, PrivateFilesMustBe0600) {
  TempDir d;
  const auto p = d.str() + "/secret";
  std::string err;
  ASSERT_TRUE(files::writePrivateAtomic(p, "x", &err)) << err;
  struct stat st{};
  ASSERT_EQ(::stat(p.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  EXPECT_EQ(files::readPrivate(p, &err), "x");
  ::chmod(p.c_str(), 0644);
  EXPECT_FALSE(files::readPrivate(p, &err));
  EXPECT_NE(err.find("0600"), std::string::npos);
  ::chmod(p.c_str(), 0640);
  EXPECT_FALSE(files::readPrivate(p, &err));
}

TEST(Token, StoreRefusesWorldReadableAndMalformedTokens) {
  TempDir d;
  const auto p = d.str() + "/device.token";
  const std::string good = "tld_" + std::string(43, 'A');
  std::string err;
  {
    std::ofstream f(p);
    f << good << "\n";
  }
  ::chmod(p.c_str(), 0644);
  TokenStore s(p);
  EXPECT_FALSE(s.load(&err));
  ::chmod(p.c_str(), 0600);
  ASSERT_TRUE(s.load(&err)) << err;
  EXPECT_EQ(s.get(), good);
  EXPECT_FALSE(s.replace("tlu_" + std::string(43, 'A'), &err));  // a user session token, not a device token
  EXPECT_FALSE(s.replace("tld_short", &err));
  EXPECT_FALSE(TokenStore::wellFormed("tld_" + std::string(42, 'A') + "!"));
  const std::string next = "tld_" + std::string(43, 'B');
  ASSERT_TRUE(s.replace(next, &err)) << err;
  TokenStore again(p);
  ASSERT_TRUE(again.load(&err));
  EXPECT_EQ(again.get(), next);
}
