// Shared fixture of the collector end-to-end tests: Xvfb, the real backend on the test database with a fake LLM,
// a registered device, and helpers to run the collector binary.
#pragma once

#include <gtest/gtest.h>
#include <json/json.h>
#include <unistd.h>

#include <ctime>

#include <filesystem>
#include <future>
#include <sqlite3.h>

#include "support/api_client.h"
#include "support/fake_llm.h"
#include "support/system.h"

using namespace thelife::test;
using thelife::collector::testing::Child;
using thelife::collector::testing::readFile;
using thelife::collector::testing::TempDir;
using thelife::collector::testing::waitUntil;
using thelife::collector::testing::XClient;
using thelife::collector::testing::Xvfb;
using namespace std::chrono_literals;

namespace {

inline Json::Value J(const std::string& text) {
  Json::Value v;
  Json::CharReaderBuilder b;
  std::string err;
  std::unique_ptr<Json::CharReader> r(b.newCharReader());
  r->parse(text.data(), text.data() + text.size(), &v, &err);
  return v;
}

// Seconds since the epoch of an RFC 3339 timestamp from the API.
inline int64_t parseTime(const std::string& s) {
  std::tm tm{};
  strptime(s.substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
  int64_t t = timegm(&tm);
  const auto z = s.find_first_of("Z+-", 19);
  if (z != std::string::npos && s[z] != 'Z') {
    const int sign = s[z] == '-' ? -1 : 1;
    t -= sign * (std::stoi(s.substr(z + 1, 2)) * 3600 + std::stoi(s.substr(z + 4, 2)) * 60);
  }
  return t;
}

inline drogon::orm::Result sql(const std::string& tenant, const std::string& q) {
  auto tx = appDb()->newTransaction();
  tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenant);
  auto r = tx->execSqlSync(q);
  std::promise<bool> committed;
  tx->setCommitCallback([&committed](bool ok) { committed.set_value(ok); });
  tx.reset();
  EXPECT_TRUE(committed.get_future().get());
  return r;
}

}  // namespace

class EndToEnd : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    xvfb_ = std::make_unique<Xvfb>();
    if (!xvfb_->ok()) GTEST_SKIP() << "Xvfb not available";
    user_ = createTenantWithUser();
    disableDefaultRoutines(user_.tenantId);
    fake_ = std::make_unique<FakeLlm>();
    server_ = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake_->baseUrl()},
                                 {"THELIFE_JOBS", "on"},
                                 {"THELIFE_JOBS_TENANT", user_.tenantId},
                                 {"THELIFE_JOBS_HOUSEKEEPING", "off"},
                                 {"THELIFE_JOBS_POLL_MS", "100"},
                                 {"THELIFE_INGEST_DERIVE_DELAY_S", "0"}});
    api_ = std::make_unique<ApiClient>(*server_, user_);
    const auto d = api_->post("/devices", J(R"({"name":"Desk","platform":"linux-x11"})"));
    ASSERT_EQ(d.status, 201) << d.body;
    token_ = d.json["token"].asString();
    deviceId_ = d.json["device"]["id"].asString();
    log_ = home_.str() + "/collector.log";
  }
  void TearDown() override {
    if (collector_) collector_->stop(SIGKILL);
    if (HasFailure()) std::cerr << "---- collector log ----\n" << readFile(log_) << "-----------------------\n";
  }
  std::map<std::string, std::string> env() {
    return {{"HOME", home_.str()},
            {"DISPLAY", xvfb_->display()},
            {"XDG_CONFIG_HOME", ""},
            {"XDG_STATE_HOME", ""},
            {"THELIFE_COLLECTOR_FAST_TIMERS", "1"},
            {"THELIFE_COLLECTOR_NO_LOGIND", "1"},
            {"DBUS_SESSION_BUS_ADDRESS", "unix:path=" + home_.str() + "/no-bus"}};
  }
  int cli(const std::vector<std::string>& args, const std::string& stdinText = "") {
    std::vector<std::string> argv{THELIFE_COLLECTOR_BIN};
    argv.insert(argv.end(), args.begin(), args.end());
    Child c(argv, env(), log_, stdinText);
    return c.waitExit(30s).value_or(-1);
  }

  TempDir home_;
  std::unique_ptr<Xvfb> xvfb_;
  TestUser user_;
  std::unique_ptr<FakeLlm> fake_;
  std::unique_ptr<ServerProcess> server_;
  std::unique_ptr<ApiClient> api_;
  std::unique_ptr<Child> collector_;
  std::string token_, deviceId_, log_;
};

