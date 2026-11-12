#include <gtest/gtest.h>

#include <set>
#include <thread>

#include "auth/rate_limiter.h"
#include "core/crypto.h"
#include "core/ids.h"
#include "core/json.h"

using namespace thelife;

TEST(Ids, UuidV7FormatVersionAndVariant) {
  const auto id = ids::newUuidV7();
  ASSERT_TRUE(ids::isUuid(id)) << id;
  EXPECT_EQ(id[14], '7');                                   // version nibble
  EXPECT_NE(std::string("89ab").find(id[19]), std::string::npos);  // RFC 4122 variant
}

TEST(Ids, UuidV7IsTimeOrderedAcrossMilliseconds) {
  const auto a = ids::newUuidV7();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto b = ids::newUuidV7();
  EXPECT_LT(a, b);
}

TEST(Ids, UuidV7IsUnique) {
  std::set<std::string> seen;
  for (int i = 0; i < 10000; ++i) seen.insert(ids::newUuidV7());
  EXPECT_EQ(seen.size(), 10000u);
}

TEST(Ids, IsUuidRejectsMalformed) {
  EXPECT_FALSE(ids::isUuid(""));
  EXPECT_FALSE(ids::isUuid("not-a-uuid"));
  EXPECT_FALSE(ids::isUuid("0193a0b4-0000-7000-8000-00000000000g"));
  EXPECT_FALSE(ids::isUuid("0193a0b400007000800000000000000000"));
  EXPECT_TRUE(ids::isUuid("0193A0B4-0000-7000-8000-000000000001"));
}

TEST(Tokens, FormatPrefixAndHash) {
  const auto t = crypto::newToken(crypto::TokenKind::Device);
  EXPECT_EQ(t.token.size(), 47u);
  EXPECT_TRUE(t.token.starts_with("tld_"));
  EXPECT_EQ(t.prefix, t.token.substr(0, 8));
  EXPECT_EQ(t.hash.size(), 32u);
  EXPECT_EQ(crypto::hashPresentedToken(t.token), t.hash);
  EXPECT_NE(crypto::newToken(crypto::TokenKind::Device).token, t.token);
}

TEST(Tokens, MalformedTokensHashToEmpty) {
  const auto good = crypto::newToken(crypto::TokenKind::UserSession).token;
  EXPECT_TRUE(crypto::hashPresentedToken("").empty());
  EXPECT_TRUE(crypto::hashPresentedToken("tlx_" + good.substr(4)).empty());   // unknown kind
  EXPECT_TRUE(crypto::hashPresentedToken(good.substr(0, 46)).empty());       // too short
  EXPECT_TRUE(crypto::hashPresentedToken(good.substr(0, 46) + "=").empty()); // bad alphabet
  EXPECT_TRUE(crypto::hashPresentedToken(good + "A").empty());               // too long
}

TEST(Passwords, HashAndVerify) {
  const auto h = crypto::hashPassword("correct horse battery staple");
  EXPECT_TRUE(h.starts_with("$argon2id$"));
  EXPECT_TRUE(crypto::verifyPassword("correct horse battery staple", h));
  EXPECT_FALSE(crypto::verifyPassword("correct horse battery stapl", h));
  EXPECT_NE(crypto::hashPassword("correct horse battery staple"), h);  // salted
}

TEST(Passwords, RandomPasswordAlphabetAndLength) {
  const auto p = crypto::randomPassword();
  EXPECT_EQ(p.size(), 20u);
  EXPECT_EQ(p.find_first_of("0O1lI"), std::string::npos);
}

TEST(Json, RejectsDuplicateKeysAndTrailingGarbage) {
  EXPECT_NO_THROW(json::parse(R"({"a":1})"));
  EXPECT_THROW(json::parse(R"({"a":1,"a":2})"), std::invalid_argument);
  EXPECT_THROW(json::parse(R"({"a":1} x)"), std::invalid_argument);
  EXPECT_THROW(json::parse("{"), std::invalid_argument);
}

TEST(FailureLimiter, BlocksAfterMaxFailuresAndResets) {
  auth::FailureLimiter l(3, std::chrono::seconds(60));
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(l.blockedFor("k"), 0);
    l.recordFailure("k");
  }
  EXPECT_GT(l.blockedFor("k"), 0);
  EXPECT_EQ(l.blockedFor("other"), 0);
  l.reset("k");
  EXPECT_EQ(l.blockedFor("k"), 0);
}

TEST(FailureLimiter, WindowExpires) {
  auth::FailureLimiter l(1, std::chrono::seconds(1));
  l.recordFailure("k");
  EXPECT_GT(l.blockedFor("k"), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_EQ(l.blockedFor("k"), 0);
}
