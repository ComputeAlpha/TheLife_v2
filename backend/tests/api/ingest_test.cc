// Collector ingest end to end (backend_lld.md §7.2, §7.4): a registered device uploads observations, gets its
// configuration with ETags, reports heartbeats and rotates its token; the derivation job turns window and idle
// observations into process detections, with the user's own rules first. Payloads are stored sealed.
#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <future>
#include <thread>

#include "core/json.h"
#include "support/api_client.h"
#include "support/fake_llm.h"

#include <filesystem>

#include <sys/inotify.h>
#include <unistd.h>

using namespace thelife;
using namespace thelife::test;

namespace {

Json::Value J(const std::string& t) { return json::parse(t); }

drogon::orm::Result sql(const std::string& tenant, const std::string& q, const std::vector<std::string>& args = {}) {
  auto tx = appDb()->newTransaction();
  tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenant);
  auto r = args.empty() ? tx->execSqlSync(q) : tx->execSqlSync(q, args[0]);
  std::promise<bool> committed;
  tx->setCommitCallback([&committed](bool ok) { committed.set_value(ok); });
  tx.reset();
  EXPECT_TRUE(committed.get_future().get());
  return r;
}

template <typename F>
bool eventually(F cond, int ms = 15000) {
  for (int i = 0; i < ms / 50; ++i) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return cond();
}

// RFC 3339 UTC, `minutes` from now (negative: in the past).
std::string at(double minutes) {
  const auto t = std::chrono::system_clock::now() + std::chrono::seconds(static_cast<int64_t>(minutes * 60));
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

Json::Value obs(int64_t seq, const std::string& capturedAt, const std::string& kind, const Json::Value& payload) {
  Json::Value o;
  o["client_seq"] = static_cast<Json::Int64>(seq);
  o["captured_at"] = capturedAt;
  o["kind"] = kind;
  o["payload"] = payload;
  return o;
}

Json::Value window(const std::string& inst, const std::string& cls, const std::string& title) {
  Json::Value p;
  p["window_id"] = "0x3a0000f";
  p["wm_class"].append(inst);
  p["wm_class"].append(cls);
  p["title"] = title;
  p["pid"] = 4242;
  p["exe"] = "/usr/bin/" + inst;
  p["monitor"] = 0;
  p["fullscreen"] = false;
  return p;
}

Json::Value batch(const std::vector<Json::Value>& items) {
  Json::Value b;
  for (const auto& i : items) b["observations"].append(i);
  return b;
}

class Ingest : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    user = createTenantWithUser();
    server = startServerWithDb({{"THELIFE_JOBS", "on"},
                                {"THELIFE_JOBS_TENANT", user.tenantId},
                                {"THELIFE_JOBS_HOUSEKEEPING", "off"},
                                {"THELIFE_JOBS_POLL_MS", "100"},
                                {"THELIFE_INGEST_DERIVE_DELAY_S", "0"}});
    api = std::make_unique<ApiClient>(*server, user);
    const auto d = api->post("/devices", J(R"({"name":"Desk","platform":"linux-x11"})"));
    ASSERT_EQ(d.status, 201) << d.body;
    deviceId = d.json["device"]["id"].asString();
    device = std::make_unique<ApiClient>(*server, user, d.json["token"].asString());
  }
  TestUser user;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api, device;
  std::string deviceId;
};

}  // namespace

TEST_F(Ingest, ObservationsAreValidatedIdempotentAndStoredSealed) {
  const auto title = "budget-2027.ods - LibreOffice Calc";
  const auto b = batch({obs(1, at(-3), "window", window("libreoffice-calc", "libreoffice-calc", title)),
                        obs(2, at(-2), "process_snapshot", J(R"({"full":true,"procs":[{"exe":"/usr/bin/bash"}]})"))});
  // The collector must send an Idempotency-Key (its seq range).
  EXPECT_EQ(device->post("/ingest/observations", b).status, 422);

  auto r = device->post("/ingest/observations", b, "device:1-2");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["accepted"].size(), 2u);
  EXPECT_EQ(r.json["duplicates"].size(), 0u);
  // Same key: the stored response is replayed. New key, same seqs: reported as duplicates, nothing stored twice.
  EXPECT_EQ(device->post("/ingest/observations", b, "device:1-2").header("Idempotent-Replayed"), "true");
  r = device->post("/ingest/observations", b, "device:1-2-retry");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.json["accepted"].size(), 0u);
  EXPECT_EQ(r.json["duplicates"].size(), 2u);
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM observations")[0]["n"].as<int>(), 2);

  // Sealed at rest: the window title is not in the database.
  const auto raw = sql(user.tenantId, "SELECT encode(payload, 'escape') AS p FROM observations WHERE client_seq = 1");
  EXPECT_EQ(raw[0]["p"].as<std::string>().find("budget"), std::string::npos);

  // Payloads are checked per kind; nothing of a bad batch is stored.
  auto bad = batch({obs(3, at(-1), "window", J(R"({"window_id":"0x1","wm_class":["a","b"]})")),
                    obs(4, at(30), "idle", J(R"({"event":"start","at":"2026-01-01T00:00:00Z"})"))});
  r = device->post("/ingest/observations", bad, "device:3-4");
  ASSERT_EQ(r.status, 422) << r.body;
  EXPECT_NE(r.body.find("/observations/0/payload"), std::string::npos);
  EXPECT_NE(r.body.find("/observations/1/captured_at"), std::string::npos);  // 30 min in the future
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM observations")[0]["n"].as<int>(), 2);

  // A device token reaches only the collector endpoints; a user cannot upload as a device.
  EXPECT_EQ(device->get("/tasks").status, 403);
  EXPECT_EQ(api->post("/ingest/observations", b, "user-key:1-2").status, 403);
}

TEST_F(Ingest, WindowsBecomeProcessDetectionsWithTheUsersRulesFirst) {
  ASSERT_EQ(api->post("/ingest/rules", J(R"({"match_wm_class":"blender","app":"Blender","category":"3d modelling",
                                            "area":"skill","confidence":0.8})")).status, 201);
  EXPECT_EQ(api->post("/ingest/rules", J(R"({"app":"x","category":"y","area":"work"})")).status, 422);

  std::vector<Json::Value> items;
  int64_t seq = 1;
  items.push_back(obs(seq++, at(-60), "session", J(R"({"event":"start","collector_version":"0.1.0"})")));
  items.push_back(obs(seq++, at(-60), "window", window("code", "Code", "main.cc \xe2\x80\x94 TheLife_v2 \xe2\x80\x94 Visual Studio Code")));
  for (int m = -59; m < -40; ++m) items.push_back(obs(seq++, at(m), "process_snapshot", J(R"({"full":false})")));
  items.push_back(obs(seq++, at(-40), "window", window("Navigator", "firefox", "Lo-fi - YouTube \xe2\x80\x94 Mozilla Firefox")));
  for (int m = -39; m < -30; ++m) items.push_back(obs(seq++, at(m), "process_snapshot", J(R"({"full":false})")));
  Json::Value idle = J(R"({"event":"start","passive":false,"locked":false})");
  idle["at"] = at(-32);  // backdated to the last input
  items.push_back(obs(seq++, at(-30), "idle", idle));
  items.push_back(obs(seq++, at(-20), "idle", J(R"({"event":"end","at":"2026-01-01T00:00:00Z"})")));
  items.push_back(obs(seq++, at(-20), "window", window("blender", "Blender", "scene.blend - Blender")));
  for (int m = -19; m < -10; ++m) items.push_back(obs(seq++, at(m), "process_snapshot", J(R"({"full":false})")));
  items.push_back(obs(seq++, at(-10), "window", window("code", "Code", "x \xe2\x80\x94 TheLife_v2 \xe2\x80\x94 Visual Studio Code")));
  items.push_back(obs(seq++, at(-9), "process_snapshot", J(R"({"full":false})")));
  ASSERT_EQ(device->post("/ingest/observations", batch(items), "device:1-" + std::to_string(seq)).status, 200);

  Json::Value list;
  ASSERT_TRUE(eventually([&] {
    list = api->get("/detections?source=process").json["items"];
    return list.size() == 3;
  })) << list;
  std::map<std::string, Json::Value> byCategory;
  for (const auto& d : list) byCategory[d["category"].asString()] = d;
  ASSERT_TRUE(byCategory.count("coding") && byCategory.count("video") && byCategory.count("3d modelling")) << list;
  EXPECT_EQ(byCategory["coding"]["summary"].asString(), "VS Code: TheLife_v2");
  EXPECT_EQ(byCategory["coding"]["area"].asString(), "work");
  EXPECT_EQ(byCategory["coding"]["evidence"]["subjects"][0]["minutes"].asInt(), 20);
  EXPECT_EQ(byCategory["coding"]["device_id"].asString(), deviceId);
  EXPECT_EQ(byCategory["coding"]["status"].asString(), "detected");
  EXPECT_EQ(byCategory["video"]["area"].asString(), "entertainment");
  EXPECT_EQ(byCategory["video"]["summary"].asString(), "YouTube: Lo-fi - YouTube");
  // Ends where the user went idle (backdated), not when the collector noticed.
  EXPECT_EQ(byCategory["video"]["ended_at"].asString().substr(0, 16), at(-32).substr(0, 16));
  EXPECT_EQ(byCategory["3d modelling"]["area"].asString(), "skill");  // the user's rule
  EXPECT_FLOAT_EQ(byCategory["3d modelling"]["confidence"].asFloat(), 0.8f);

  // The last VS Code span is still open: nothing for it yet, and a second run does not duplicate anything.
  const auto cursor = sql(user.tenantId, "SELECT derived_until::text AS u FROM ingest_state")[0]["u"].as<std::string>();
  ASSERT_EQ(device->post("/ingest/observations",
                         batch({obs(seq, at(-8), "process_snapshot", J(R"({"full":false})"))}),
                         "device:" + std::to_string(seq) + "-" + std::to_string(seq))
                .status,
            200);
  ASSERT_TRUE(eventually([&] {
    return sql(user.tenantId, "SELECT count(*) AS n FROM ops.jobs WHERE kind = 'ingest.derive' "
                               "AND status IN ('queued', 'running') AND tenant_id = app.current_tenant()")
               [0]["n"].as<int>() == 0;
  }));
  EXPECT_EQ(api->get("/detections?source=process").json["items"].size(), 3u);
  EXPECT_EQ(sql(user.tenantId, "SELECT derived_until::text AS u FROM ingest_state")[0]["u"].as<std::string>(), cursor);
}

TEST_F(Ingest, ConfigHeartbeatAndTokenRotation) {
  auto c = device->get("/devices/self/config");
  ASSERT_EQ(c.status, 200) << c.body;
  EXPECT_FALSE(c.json["enabled"].asBool());  // no vision model in this test server
  EXPECT_GE(c.json["capture_interval_s"].asInt(), 60);
  EXPECT_EQ(c.json["capture_monitors"].asString(), "all");
  const auto etag = c.header("ETag");
  ASSERT_FALSE(etag.empty());
  EXPECT_EQ(device->call("GET", "/devices/self/config", Json::Value(), {{"If-None-Match", etag}}).status, 304);
  sql(user.tenantId, "UPDATE tenants SET settings = jsonb_set(settings, '{vision}', '{\"capture_monitors\":\"active\"}')");
  c = device->call("GET", "/devices/self/config", Json::Value(), {{"If-None-Match", etag}});
  ASSERT_EQ(c.status, 200);
  EXPECT_EQ(c.json["capture_monitors"].asString(), "active");
  EXPECT_NE(c.header("ETag"), etag);

  EXPECT_EQ(device->post("/devices/self/heartbeat",
                         J(R"({"version":"0.1.0","queue_depth":12,"x_ok":true,"paused_until":null,"last_error":null})"))
                .status,
            204);
  const auto d = api->get("/devices/" + deviceId).json;
  EXPECT_EQ(d["collector_version"].asString(), "0.1.0");
  EXPECT_EQ(d["status"]["queue_depth"].asInt(), 12);
  EXPECT_FALSE(d["last_seen_at"].isNull());

  // Rotation: the new token works at once, the old one for 5 more minutes.
  const auto rot = device->post("/devices/self/token:rotate", Json::Value(Json::objectValue));
  ASSERT_EQ(rot.status, 200) << rot.body;
  ApiClient fresh(*server, user, rot.json["token"].asString());
  EXPECT_EQ(fresh.get("/devices/self/config").status, 200);
  EXPECT_EQ(device->get("/devices/self/config").status, 200);
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM credentials WHERE device_id IS NOT NULL "
                               "AND expires_at BETWEEN now() + interval '4 minutes' AND now() + interval '5 minutes'")
                [0]["n"].as<int>(),
            1);
  // Revoking the device ends both.
  ASSERT_EQ(api->del("/devices/" + deviceId, "").status, 204);
  EXPECT_EQ(fresh.get("/devices/self/config").status, 401);
}

TEST_F(Ingest, OldObservationsAreRemovedAfter30Days) {
  sql(user.tenantId,
      "INSERT INTO observations (id, tenant_id, device_id, client_seq, captured_at, kind, payload) VALUES "
      "(gen_random_uuid(), app.current_tenant(), '" + deviceId + "', 1, now() - interval '31 days', 'gap', '\\x01'), "
      "(gen_random_uuid(), app.current_tenant(), '" + deviceId + "', 2, now() - interval '29 days', 'gap', '\\x01')");
  sql(user.tenantId, "SELECT ops.observations_retention()");
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM observations")[0]["n"].as<int>(), 1);
}

// ---------------------------------------------------------------- captures (vision)

namespace {

const std::string kBoundary = "thelife-test-boundary-7d1";

std::string jpeg(std::size_t n) {
  std::string b("\xFF\xD8\xFF\xE0", 4);
  b.append(n, 'x');
  return b + std::string("\xFF\xD9", 2);
}

// One "context" field and one "images" part per image.
std::string multipart(const Json::Value& context, const std::vector<std::string>& images, const char* ctype = "image/jpeg") {
  std::string body = "--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"context\"\r\n\r\n" +
                     json::compact(context) + "\r\n";
  for (std::size_t i = 0; i < images.size(); ++i) {
    body += "--" + kBoundary + "\r\nContent-Disposition: form-data; name=\"images\"; filename=\"m" +
            std::to_string(i) + ".jpg\"\r\nContent-Type: " + ctype + "\r\n\r\n" + images[i] + "\r\n";
  }
  return body + "--" + kBoundary + "--\r\n";
}

class Captures : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    uploads = std::filesystem::temp_directory_path() / ("thelife-test-uploads-" + std::to_string(::getpid()));
    user = createTenantWithUser();
    fake = std::make_unique<FakeLlm>();
    server = startServerWithDb({{"THELIFE_LLM_BASE_URL", fake->baseUrl()},
                                {"THELIFE_VISION_QUEUE", "1"},
                                {"THELIFE_UPLOAD_DIR", uploads.string()}});
    api = std::make_unique<ApiClient>(*server, user);
    const auto d = api->post("/devices", J(R"({"name":"Desk","platform":"linux-x11"})"));
    token = d.json["token"].asString();
    deviceId = d.json["device"]["id"].asString();
  }
  void TearDown() override {
    if (!uploads.empty()) std::filesystem::remove_all(uploads);
  }
  HttpResult upload(const std::string& body, const std::string& key) {
    Request r;
    r.method = "POST";
    r.url = server->baseUrl() + "/api/v1/ingest/captures";
    r.bearer = token;
    r.body = body;
    r.contentType = "multipart/form-data; boundary=" + kBoundary;
    r.headers.emplace_back("Idempotency-Key", key);
    return send(r);
  }
  Json::Value context(double minutes = 0) {
    Json::Value c;
    c["captured_at"] = at(minutes);
    c["monitors"] = J(R"([{"index":0,"width":1920,"height":1080},{"index":1,"width":1920,"height":1080}])");
    c["active_monitor"] = 0;
    c["window"] = window("code", "Code", "main.cc \xe2\x80\x94 TheLife_v2 \xe2\x80\x94 Visual Studio Code");
    c["idle_ms"] = 1200;
    c["processes"] = J(R"([{"exe":"/usr/share/code/code","cpu_pct":12.5},{"exe":"/usr/bin/firefox","cpu_pct":3}])");
    return c;
  }
  std::filesystem::path uploads;
  TestUser user;
  std::unique_ptr<FakeLlm> fake;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
  std::string token, deviceId;
};

// Watches every directory under `dir` for files being created (Drogon removes a spilled body file after the
// request, so only watching while it runs shows whether one was written).
class CreateWatch {
 public:
  explicit CreateWatch(const std::filesystem::path& dir) : fd_(inotify_init1(IN_NONBLOCK)) {
    add(dir);
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
      if (e.is_directory()) add(e.path());
    }
  }
  ~CreateWatch() { close(fd_); }
  int created() {
    int n = 0;
    alignas(inotify_event) char buf[64 * 1024];
    for (ssize_t len; (len = read(fd_, buf, sizeof buf)) > 0;) {
      for (char* p = buf; p < buf + len;) {
        const auto* ev = reinterpret_cast<const inotify_event*>(p);
        if (ev->mask & (IN_CREATE | IN_CLOSE_WRITE)) ++n;
        p += sizeof(inotify_event) + ev->len;
      }
    }
    return n;
  }
  int watches() const { return watches_; }

 private:
  void add(const std::filesystem::path& d) {
    if (inotify_add_watch(fd_, d.c_str(), IN_CREATE | IN_CLOSE_WRITE) >= 0) ++watches_;
  }
  int fd_;
  int watches_ = 0;
};

}  // namespace

TEST_F(Captures, ACaptureBecomesAVisionDetectionAndNeverTouchesDisk) {
  fake->script(J(R"([{"content":"","delay_ms":300,"tool_calls":[{"name":"submit_vision_analysis","arguments":{
      "area":"work","category":"coding","summary":"Writing ingest tests for TheLife in VS Code","confidence":0.85,
      "apps":["VS Code"]}}]}])"));
  // Large enough that Drogon's default (64 KiB in memory) would have spilled the body to a file under the upload
  // path; the server keeps it in memory.
  ASSERT_TRUE(std::filesystem::exists(uploads));
  CreateWatch watch(uploads);
  ASSERT_GT(watch.watches(), 1);
  const auto r = upload(multipart(context(), {jpeg(300 * 1024), jpeg(200 * 1024)}), "capture-0001");
  ASSERT_EQ(r.status, 202) << r.body;
  const auto captureId = r.json["capture_id"].asString();
  // The single slot is taken while the model works: the next capture is refused, not queued.
  const auto busy = upload(multipart(context(), {jpeg(1000)}), "capture-0002");
  EXPECT_EQ(busy.status, 503) << busy.body;
  EXPECT_FALSE(busy.header("Retry-After").empty());

  Json::Value list;
  ASSERT_TRUE(eventually([&] {
    list = api->get("/detections?source=vision").json["items"];
    return list.size() == 1;
  }));
  const auto& d = list[0];
  EXPECT_EQ(d["area"].asString(), "work");
  EXPECT_EQ(d["summary"].asString(), "Writing ingest tests for TheLife in VS Code");
  EXPECT_EQ(d["evidence"]["capture_id"].asString(), captureId);
  EXPECT_EQ(d["evidence"]["monitors"].asInt(), 2);
  EXPECT_EQ(d["device_id"].asString(), deviceId);
  EXPECT_EQ(d["evidence"].toStyledString().find("base64"), std::string::npos);

  // The model got both images and the context (focused window, processes), nothing else about the screen.
  const auto req = fake->requests()[0];
  const auto& content = req["messages"][1]["content"];
  ASSERT_TRUE(content.isArray());
  int images = 0;
  std::string text;
  for (const auto& part : content) {
    if (part["type"].asString() == "image_url") {
      ++images;
      EXPECT_EQ(part["image_url"]["url"].asString().rfind("data:image/jpeg;base64,/9j/", 0), 0u);
    } else {
      text += part["text"].asString();
    }
  }
  EXPECT_EQ(images, 2);
  EXPECT_NE(text.find("TheLife_v2"), std::string::npos);
  EXPECT_NE(text.find("code"), std::string::npos);

  // Nothing of the capture was written to disk or to the database.
  EXPECT_EQ(watch.created(), 0);
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM llm_calls WHERE purpose = 'vision.analyze' AND images = 2")
                [0]["n"].as<int>(),
            1);
  // A slot is free again.
  ASSERT_TRUE(eventually([&] { return upload(multipart(context(), {jpeg(1000)}), "capture-0003").status == 202; }));
}

TEST_F(Captures, BadCapturesAreRefusedAndCaptureCanBeOff) {
  EXPECT_EQ(upload(multipart(context(), {"not a jpeg"}), "capture-0010").status, 422);
  EXPECT_EQ(upload(multipart(context(), {}), "capture-0011").status, 422);
  EXPECT_EQ(upload(multipart(context(-30), {jpeg(10)}), "capture-0012").status, 422);  // stale
  // A JSON body is not a capture.
  auto r = sendJson("POST", server->baseUrl() + "/api/v1/ingest/captures", context(), token);
  EXPECT_EQ(r.status, 415);
  // A user token cannot upload captures.
  Request userReq;
  userReq.method = "POST";
  userReq.url = server->baseUrl() + "/api/v1/ingest/captures";
  userReq.bearer = api->token();
  userReq.body = multipart(context(), {jpeg(10)});
  userReq.contentType = "multipart/form-data; boundary=" + kBoundary;
  userReq.headers.emplace_back("Idempotency-Key", "user-capture-1");
  EXPECT_EQ(send(userReq).status, 403);

  // Capture switched off for the tenant: the collector is told its config is stale.
  sql(user.tenantId, "UPDATE tenants SET settings = jsonb_set(settings, '{vision}', '{\"enabled\":false}')");
  r = upload(multipart(context(), {jpeg(10)}), "capture-0013");
  EXPECT_EQ(r.status, 409) << r.body;
  EXPECT_NE(r.body.find("config-stale"), std::string::npos);
  EXPECT_FALSE(ApiClient(*server, user, token).get("/devices/self/config").json["enabled"].asBool());
  EXPECT_TRUE(fake->requests().empty());
}
