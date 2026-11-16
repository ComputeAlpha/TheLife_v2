// The collector against the real backend (Postgres + fake LLM): enrolment, observations stored, a capture turned
// into a vision detection, heartbeat status and token rotation (collector_lld.md §11 "e2e"). Skipped without the
// test database (THELIFE_TEST_DB_URL / THELIFE_TEST_ADMIN_DB_URL; see scripts/db.sh test).
#include "e2e/fixture.h"

TEST_F(EndToEnd, ACollectorUnderXReportsToTheBackend) {
  fake_->script(J(R"([{"content":"","tool_calls":[{"name":"submit_vision_analysis","arguments":{
      "area":"work","category":"coding","summary":"Editing main.cc of TheLife in VS Code","confidence":0.8,
      "apps":["VS Code"]}}]}])"));
  ASSERT_EQ(cli({"enroll", "--url", server_->baseUrl()}, token_ + "\n"), 0) << readFile(log_);
  // An old token: the collector rotates it on its own.
  {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open((home_.str() + "/.local/state/thelife-collector/queue.db").c_str(), &db), SQLITE_OK);
    sqlite3_exec(db, "UPDATE meta SET value = '2020-01-01T00:00:00.000Z' WHERE key = 'token_issued_at'", nullptr,
                 nullptr, nullptr);
    sqlite3_close(db);
  }
  XClient x(xvfb_->display());
  x.activate(x.createWindow(100, 100, 1200, 800, 0x1e1e1e, "code", "main.cc — TheLife_v2 — Visual Studio Code",
                            static_cast<uint32_t>(::getpid())));
  collector_ = std::make_unique<Child>(std::vector<std::string>{THELIFE_COLLECTOR_BIN, "run"}, env(), log_);

  // The capture becomes a vision detection; the images went to the model, nowhere else.
  Json::Value list;
  ASSERT_TRUE(waitUntil(
      [&] {
        list = api_->get("/detections?source=vision").json["items"];
        return list.size() == 1;
      },
      40s));
  EXPECT_EQ(list[0]["summary"].asString(), "Editing main.cc of TheLife in VS Code");
  EXPECT_EQ(list[0]["device_id"].asString(), deviceId_);
  EXPECT_EQ(list[0]["evidence"]["monitors"].asInt(), 2);
  const auto content = fake_->requests()[0]["messages"][1]["content"];
  int images = 0;
  for (const auto& part : content) images += part["type"].asString() == "image_url";
  EXPECT_EQ(images, 2);

  // Observations were accepted and stored (sealed), including the capture's own record.
  ASSERT_TRUE(waitUntil(
      [&] {
        const auto r = sql(user_.tenantId,
                           "SELECT count(DISTINCT kind) AS k FROM observations WHERE kind IN "
                           "('session', 'window', 'process_snapshot', 'capture_meta')");
        return r[0]["k"].as<int>() == 4;
      },
      30s));

  // Rotated: a new token on disk, the old one expiring, and the collector still heard from.
  ASSERT_TRUE(waitUntil([&] { return readFile(home_.str() + "/.config/thelife-collector/device.token") != token_ + "\n"; },
                        20s));
  const auto creds = sql(user_.tenantId,
                         "SELECT count(*) FILTER (WHERE expires_at IS NULL) AS live, count(*) AS n FROM credentials "
                         "WHERE device_id = '" + deviceId_ + "'");
  EXPECT_EQ(creds[0]["n"].as<int>(), 2);
  EXPECT_EQ(creds[0]["live"].as<int>(), 1);
  const auto before = api_->get("/devices/" + deviceId_).json["last_seen_at"].asString();
  ASSERT_TRUE(waitUntil([&] { return api_->get("/devices/" + deviceId_).json["last_seen_at"].asString() != before; },
                        15s));
  const auto dev = api_->get("/devices/" + deviceId_).json;
  EXPECT_FALSE(dev["collector_version"].asString().empty());
  EXPECT_TRUE(dev["status"]["x_ok"].asBool());
  EXPECT_TRUE(dev["status"]["last_capture_at"].isString());
  EXPECT_EQ(collector_->stop(SIGTERM), 0);
  EXPECT_EQ(readFile(log_).find(token_), std::string::npos);
}
