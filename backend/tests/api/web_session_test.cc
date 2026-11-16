// The web UI's cookie session and static web root (frontend_lld.md §2), against the real binary and database.
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "support/api_client.h"
#include "support/fixtures.h"
#include "support/http.h"
#include "support/server_process.h"

using namespace thelife::test;
namespace fs = std::filesystem;

namespace {

std::string cookieValue(const HttpResult& r, const std::string& name) {
  for (const auto& c : r.setCookies) {
    if (c.starts_with(name + "=")) return c.substr(name.size() + 1, c.find(';') - name.size() - 1);
  }
  return "<none>";
}

std::string cookieLine(const HttpResult& r, const std::string& name) {
  for (const auto& c : r.setCookies) {
    if (c.starts_with(name + "=")) return c;
  }
  return {};
}

bool contains(const std::string& hay, const std::string& needle) {
  auto lower = [](std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
  };
  return lower(hay).find(lower(needle)) != std::string::npos;
}

class WebSession : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    server_ = startServerWithDb();
    user_ = createTenantWithUser();
  }
  std::string url(const std::string& p) const { return server_->baseUrl() + "/api/v1" + p; }

  HttpResult webLogin(const std::string& password) {
    Json::Value b;
    b["email"] = user_.email;
    b["password"] = password;
    return sendJson("POST", url("/auth/web-session"), b);
  }
  std::string signIn() {
    auto r = webLogin(user_.password);
    EXPECT_EQ(r.status, 200) << r.body;
    return cookieValue(r, "tl_session");
  }
  HttpResult withCookie(const std::string& method, const std::string& path, const std::string& cookie,
                        bool csrf, const std::string& body = {}) {
    Request r;
    r.method = method;
    r.url = url(path);
    r.headers.push_back({"Cookie", "tl_session=" + cookie});
    if (csrf) r.headers.push_back({"X-TheLife-CSRF", "1"});
    r.body = body;
    return send(r);
  }

  std::unique_ptr<ServerProcess> server_;
  TestUser user_;
};

TEST_F(WebSession, LoginSetsAHardenedCookieAndKeepsTheTokenOutOfTheBody) {
  auto r = webLogin(user_.password);
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_FALSE(r.json.isMember("token"));
  EXPECT_EQ(r.json["user"]["id"].asString(), user_.userId);
  EXPECT_FALSE(r.json["expires_at"].asString().empty());
  const auto line = cookieLine(r, "tl_session");
  ASSERT_FALSE(line.empty()) << "no tl_session cookie";
  EXPECT_TRUE(cookieValue(r, "tl_session").starts_with("tlu_"));
  EXPECT_TRUE(contains(line, "HttpOnly")) << line;
  EXPECT_TRUE(contains(line, "Secure")) << line;
  EXPECT_TRUE(contains(line, "SameSite=Strict")) << line;
  EXPECT_TRUE(contains(line, "Path=/api")) << line;
  EXPECT_TRUE(contains(line, "Max-Age=2592000")) << line;
  EXPECT_EQ(r.header("cache-control"), "no-store");
}

TEST_F(WebSession, WrongPasswordIs401WithoutACookie) {
  auto r = webLogin("wrong password");
  EXPECT_EQ(r.status, 401) << r.body;
  EXPECT_TRUE(r.setCookies.empty());
}

TEST_F(WebSession, CookieAuthenticatesReads) {
  const auto c = signIn();
  auto r = withCookie("GET", "/me", c, false);
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["id"].asString(), user_.userId);
}

TEST_F(WebSession, CookieWritesNeedTheCsrfHeader) {
  const auto c = signIn();
  const std::string body = R"({"title":"From the browser","area":"personal"})";
  auto blocked = withCookie("POST", "/tasks", c, false, body);
  EXPECT_EQ(blocked.status, 403) << blocked.body;
  EXPECT_NE(blocked.json["type"].asString().find("csrf-required"), std::string::npos) << blocked.body;
  auto ok = withCookie("POST", "/tasks", c, true, body);
  EXPECT_EQ(ok.status, 201) << ok.body;
  // Wrong header value is not enough.
  Request r;
  r.method = "POST";
  r.url = url("/tasks");
  r.headers = {{"Cookie", "tl_session=" + c}, {"X-TheLife-CSRF", "yes"}};
  r.body = body;
  EXPECT_EQ(send(r).status, 403);
}

TEST_F(WebSession, BearerRequestsIgnoreTheCookieAndNeedNoCsrfHeader) {
  ApiClient api(*server_, user_);
  Request r;
  r.method = "POST";
  r.url = url("/tasks");
  r.bearer = api.token();
  r.headers = {{"Cookie", "tl_session=tlu_garbage"}};
  r.body = R"({"title":"From a script","area":"personal"})";
  EXPECT_EQ(send(r).status, 201);
}

TEST_F(WebSession, OnlyUserSessionsAreAcceptedFromTheCookie) {
  ApiClient api(*server_, user_);
  Json::Value dev;
  dev["name"] = "laptop";
  dev["platform"] = "linux-x11";
  auto d = api.post("/devices", dev);
  ASSERT_EQ(d.status, 201) << d.body;
  EXPECT_EQ(withCookie("GET", "/me", d.json["token"].asString(), false).status, 401);

  Json::Value key;
  key["name"] = "script";
  key["scopes"].append("tasks:read");
  auto k = api.post("/api-keys", key);
  ASSERT_EQ(k.status, 201) << k.body;
  EXPECT_EQ(withCookie("GET", "/tasks", k.json["token"].asString(), false).status, 401);
  EXPECT_EQ(withCookie("GET", "/me", "tlu_not-a-real-token", false).status, 401);
}

TEST_F(WebSession, LogoutRevokesTheSessionAndClearsTheCookie) {
  const auto c = signIn();
  auto noCsrf = withCookie("DELETE", "/auth/web-session", c, false);
  EXPECT_EQ(noCsrf.status, 403);
  auto out = withCookie("DELETE", "/auth/web-session", c, true);
  ASSERT_EQ(out.status, 204) << out.body;
  const auto line = cookieLine(out, "tl_session");
  ASSERT_FALSE(line.empty());
  EXPECT_EQ(cookieValue(out, "tl_session"), "");
  EXPECT_TRUE(contains(line, "Max-Age=0")) << line;
  EXPECT_EQ(withCookie("GET", "/me", c, false).status, 401);
}

TEST(WebApiErrors, UnknownApiPathIsProblemJson) {
  ServerProcess server;
  auto r = httpGet(server.baseUrl() + "/api/v1/no-such-thing");
  EXPECT_EQ(r.status, 404);
  EXPECT_NE(r.contentType.find("application/problem+json"), std::string::npos) << r.contentType;
  EXPECT_FALSE(r.json["request_id"].asString().empty());
}

TEST(WebApiErrors, WorkingDirectoryFilesAreNeverServed) {
  // Drogon's default static handler would serve e.g. CMakeLists.txt from the working directory.
  const auto dir = fs::temp_directory_path() / ("thelife-cwd-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "secret.txt") << "do not serve";
  const auto old = fs::current_path();
  fs::current_path(dir);  // the server process inherits it
  ServerProcess server;
  fs::current_path(old);
  auto r = httpGet(server.baseUrl() + "/secret.txt");
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body.find("do not serve"), std::string::npos);
  fs::remove_all(dir);
}

class WebRoot : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = fs::temp_directory_path() / ("thelife-web-" + std::to_string(::getpid()));
    root_ = base_ / "dist";
    fs::create_directories(root_ / "assets");
    std::ofstream(root_ / "index.html") << "<!doctype html><title>TheLife</title><div id=root></div>";
    std::ofstream(root_ / "assets" / "app-1a2b3c.js") << "console.log('app')";
    std::ofstream(root_ / "favicon.svg") << "<svg xmlns='http://www.w3.org/2000/svg'/>";
    std::ofstream(root_ / ".env") << "SECRET=1";
    std::ofstream(base_ / "outside.txt") << "outside the root";
    fs::create_symlink(base_ / "outside.txt", root_ / "escape.txt");
    server_ = std::make_unique<ServerProcess>(std::map<std::string, std::string>{{"THELIFE_WEB_ROOT", root_}});
  }
  void TearDown() override {
    server_.reset();
    fs::remove_all(base_);
  }
  HttpResult get(const std::string& path) { return httpGet(server_->baseUrl() + path); }

  fs::path base_, root_;
  std::unique_ptr<ServerProcess> server_;
};

TEST_F(WebRoot, ServesIndexWithSecurityHeaders) {
  auto r = get("/");
  ASSERT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("<title>TheLife</title>"), std::string::npos);
  EXPECT_NE(r.contentType.find("text/html"), std::string::npos);
  EXPECT_EQ(r.header("cache-control"), "no-cache");
  const auto csp = r.header("content-security-policy");
  EXPECT_NE(csp.find("default-src 'self'"), std::string::npos) << csp;
  EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos) << csp;
  EXPECT_NE(csp.find("script-src 'self';"), std::string::npos) << csp;  // never inline scripts
  EXPECT_EQ(r.header("x-content-type-options"), "nosniff");
  EXPECT_EQ(r.header("referrer-policy"), "no-referrer");
}

TEST_F(WebRoot, ClientRoutesFallBackToIndex) {
  for (const auto* p : {"/calendar/week/2026-11-16", "/chat", "/settings/devices"}) {
    auto r = get(p);
    EXPECT_EQ(r.status, 200) << p;
    EXPECT_NE(r.body.find("<title>TheLife</title>"), std::string::npos) << p;
  }
}

TEST_F(WebRoot, HashedAssetsAreImmutable) {
  auto r = get("/assets/app-1a2b3c.js");
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.body, "console.log('app')");
  EXPECT_NE(r.contentType.find("javascript"), std::string::npos) << r.contentType;
  EXPECT_EQ(r.header("cache-control"), "public, max-age=31536000, immutable");
  auto svg = get("/favicon.svg");
  EXPECT_EQ(svg.status, 200);
  EXPECT_EQ(svg.header("cache-control"), "no-cache");
}

TEST_F(WebRoot, MissingFilesWithAnExtensionAre404) {
  auto r = get("/assets/app-old.js");
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body.find("<title>TheLife</title>"), std::string::npos);
}

TEST_F(WebRoot, NothingOutsideTheRootOrHiddenIsServed) {
  EXPECT_EQ(get("/escape.txt").status, 404);  // symlink out of the root
  EXPECT_EQ(get("/.env").body.find("SECRET"), std::string::npos);  // dotfiles are hidden (index.html instead)
  EXPECT_EQ(get("/../outside.txt").body.find("outside the root"), std::string::npos);
  EXPECT_EQ(get("/assets/%2e%2e/%2e%2e/outside.txt").body.find("outside the root"), std::string::npos);
}

TEST_F(WebRoot, ApiAndHealthAreNotShadowed) {
  auto api = get("/api/v1/me");
  EXPECT_EQ(api.status, 401);
  EXPECT_NE(api.contentType.find("application/problem+json"), std::string::npos);
  EXPECT_EQ(get("/api/v1/nothing-here").status, 404);
  EXPECT_EQ(get("/healthz").status, 200);
}

}  // namespace
