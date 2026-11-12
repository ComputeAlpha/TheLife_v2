// Functional tests for the auth endpoints (api/resources/auth.yaml), run against the real binary and database.
#include <gtest/gtest.h>

#include "support/fixtures.h"
#include "support/http.h"
#include "support/server_process.h"

using namespace thelife::test;

namespace {

class AuthApi : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    server_ = std::make_unique<ServerProcess>(std::map<std::string, std::string>{{"THELIFE_DB_URL", appDbUrl()}});
    user_ = createTenantWithUser();
  }
  std::string url(const std::string& p) const { return server_->baseUrl() + "/api/v1" + p; }

  HttpResult login(const std::string& email, const std::string& password) {
    Json::Value b;
    b["email"] = email;
    b["password"] = password;
    return sendJson("POST", url("/auth/login"), b);
  }
  std::string loginToken(const TestUser& u) {
    auto r = login(u.email, u.password);
    EXPECT_EQ(r.status, 200) << r.body;
    return r.json["token"].asString();
  }
  static void expectProblem(const HttpResult& r, int status) {
    EXPECT_EQ(r.status, status) << r.body;
    EXPECT_NE(r.contentType.find("application/problem+json"), std::string::npos) << r.contentType;
    EXPECT_EQ(r.json["status"].asInt(), status);
    EXPECT_FALSE(r.json["type"].asString().empty());
    EXPECT_FALSE(r.json["request_id"].asString().empty());
    EXPECT_EQ(r.json["request_id"].asString(), r.header("x-request-id"));
  }

  std::unique_ptr<ServerProcess> server_;
  TestUser user_;
};

TEST_F(AuthApi, LoginReturnsSessionTokenAndUser) {
  auto r = login(user_.email, user_.password);
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_TRUE(r.json["token"].asString().starts_with("tlu_"));
  EXPECT_EQ(r.json["token"].asString().size(), 47u);
  EXPECT_FALSE(r.json["expires_at"].asString().empty());
  EXPECT_EQ(r.json["user"]["id"].asString(), user_.userId);
  EXPECT_EQ(r.json["user"]["email"].asString(), user_.email);
  EXPECT_EQ(r.json["user"]["day_start_hour"].asInt(), 4);
  EXPECT_FALSE(r.json["user"].isMember("password_hash"));
  EXPECT_EQ(r.header("cache-control"), "no-store");
}

TEST_F(AuthApi, LoginEmailIsCaseInsensitive) {
  std::string upper = user_.email;
  for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_EQ(login(upper, user_.password).status, 200);
}

TEST_F(AuthApi, WrongPasswordAndUnknownEmailLookIdentical) {
  auto wrong = login(user_.email, "nope");
  auto unknown = login("nobody-" + user_.email, "nope");
  expectProblem(wrong, 401);
  expectProblem(unknown, 401);
  EXPECT_EQ(wrong.json["detail"], unknown.json["detail"]);
}

TEST_F(AuthApi, LoginValidation) {
  Json::Value b;
  b["email"] = user_.email;
  expectProblem(sendJson("POST", url("/auth/login"), b), 422);  // password missing
  b["password"] = "x";
  b["extra"] = true;
  expectProblem(sendJson("POST", url("/auth/login"), b), 422);  // unknown property
  Request raw;
  raw.method = "POST";
  raw.url = url("/auth/login");
  raw.body = "{not json";
  expectProblem(send(raw), 400);
  raw.body = R"({"email":"a@b.c","password":"x"})";
  raw.contentType = "text/plain";
  expectProblem(send(raw), 415);
}

TEST_F(AuthApi, RepeatedFailuresAreRateLimited) {
  for (int i = 0; i < 5; ++i) EXPECT_EQ(login(user_.email, "wrong").status, 401);
  auto r = login(user_.email, user_.password);  // even the right password is refused while locked
  expectProblem(r, 429);
  EXPECT_GT(std::stoi(r.header("retry-after")), 0);
}

TEST_F(AuthApi, MeRequiresValidToken) {
  expectProblem(httpGet(url("/me")), 401);
  Request r;
  r.url = url("/me");
  r.bearer = "tlu_short";
  expectProblem(send(r), 401);
  r.bearer = "tlu_" + std::string(43, 'A');  // well-formed but unknown
  expectProblem(send(r), 401);
}

TEST_F(AuthApi, MeReturnsTheCallingUser) {
  Request r;
  r.url = url("/me");
  r.bearer = loginToken(user_);
  auto me = send(r);
  ASSERT_EQ(me.status, 200) << me.body;
  EXPECT_EQ(me.json["id"].asString(), user_.userId);
  EXPECT_EQ(me.json["workdays"].size(), 5u);
}

TEST_F(AuthApi, LogoutRevokesOnlyThatSession) {
  const auto a = loginToken(user_);
  const auto b = loginToken(user_);
  Request out;
  out.method = "POST";
  out.url = url("/auth/logout");
  out.bearer = a;
  EXPECT_EQ(send(out).status, 204);
  Request me;
  me.url = url("/me");
  me.bearer = a;
  expectProblem(send(me), 401);
  me.bearer = b;
  EXPECT_EQ(send(me).status, 200);
}

TEST_F(AuthApi, UpdateMeValidatesAndPersists) {
  const auto token = loginToken(user_);
  Json::Value patch;
  patch["timezone"] = "Asia/Kolkata";
  patch["day_start_hour"] = 5;
  patch["workdays"].append("MO");
  patch["workdays"].append("SA");
  auto r = sendJson("PATCH", url("/me"), patch, token, "application/merge-patch+json");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["timezone"].asString(), "Asia/Kolkata");
  EXPECT_EQ(r.json["day_start_hour"].asInt(), 5);
  EXPECT_EQ(r.json["workdays"].size(), 2u);

  Json::Value bad;
  bad["timezone"] = "Mars/Olympus";
  auto e = sendJson("PATCH", url("/me"), bad, token, "application/merge-patch+json");
  expectProblem(e, 422);
  EXPECT_EQ(e.json["errors"][0]["path"].asString(), "/timezone");

  Json::Value range;
  range["day_start_hour"] = 13;
  expectProblem(sendJson("PATCH", url("/me"), range, token, "application/merge-patch+json"), 422);
  Json::Value role;
  role["role"] = "tenant_admin";  // not writable
  expectProblem(sendJson("PATCH", url("/me"), role, token, "application/merge-patch+json"), 422);
}

TEST_F(AuthApi, ChangePasswordRevokesOtherSessions) {
  const auto keep = loginToken(user_);
  const auto other = loginToken(user_);
  Json::Value b;
  b["current_password"] = "wrong";
  b["new_password"] = "a brand new long password";
  expectProblem(sendJson("POST", url("/me/password"), b, keep), 422);
  b["current_password"] = user_.password;
  ASSERT_EQ(sendJson("POST", url("/me/password"), b, keep).status, 204);

  Request me;
  me.url = url("/me");
  me.bearer = keep;
  EXPECT_EQ(send(me).status, 200);
  me.bearer = other;
  expectProblem(send(me), 401);
  expectProblem(login(user_.email, user_.password), 401);
  EXPECT_EQ(login(user_.email, "a brand new long password").status, 200);
}

TEST_F(AuthApi, SessionsAreTenantScoped) {
  // A second tenant's user sees only their own profile with their own token.
  auto other = createTenantWithUser();
  Request me;
  me.url = url("/me");
  me.bearer = loginToken(other);
  auto r = send(me);
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.json["id"].asString(), other.userId);
  EXPECT_NE(r.json["id"].asString(), user_.userId);
}

}  // namespace
