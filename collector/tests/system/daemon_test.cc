// The collector binary end to end, under Xvfb, against tests/fixtures/fake_backend.py (collector_lld.md §11:
// offline, notifications, hostile backend, security).
#include <gtest/gtest.h>
#include <sys/inotify.h>
#include <systemd/sd-bus.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "app/state.h"
#include "core/files.h"
#include "core/json.h"
#include "queue/queue.h"
#include "support/schema.h"
#include "support/system.h"

using namespace thelife::collector;
using namespace thelife::collector::testing;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string kOtherToken() { return "tld_" + std::string(43, 'Z'); }

bool hasJpegMagic(const std::string& bytes) { return bytes.find("\xFF\xD8\xFF") != std::string::npos; }

// Records files created or written in a set of directories (inotify, not recursive).
class DiskWatch {
 public:
  explicit DiskWatch(const std::vector<std::string>& dirs) {
    fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    for (const auto& d : dirs) {
      const int wd = ::inotify_add_watch(fd_, d.c_str(), IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MODIFY);
      if (wd >= 0) dirs_[wd] = d;
    }
    thread_ = std::thread([this] {
      alignas(inotify_event) char buf[64 * 1024];
      while (!stop_) {
        const auto n = ::read(fd_, buf, sizeof buf);
        if (n <= 0) {
          std::this_thread::sleep_for(5ms);
          continue;
        }
        for (char* p = buf; p < buf + n;) {
          const auto* e = reinterpret_cast<inotify_event*>(p);
          if (e->len > 0) {
            std::lock_guard lock(mu_);
            const auto path = dirs_[e->wd] + "/" + e->name;
            touched_.insert(path);
            // Read what was written while it still exists (temporary files vanish fast).
            if (e->mask & (IN_CLOSE_WRITE | IN_MODIFY)) {
              std::ifstream f(path, std::ios::binary);
              std::string head(4096, '\0');
              f.read(head.data(), static_cast<std::streamsize>(head.size()));
              head.resize(static_cast<size_t>(f.gcount()));
              if (hasJpegMagic(head)) jpeg_.insert(path);
            }
          }
          p += sizeof(inotify_event) + e->len;
        }
      }
    });
  }
  ~DiskWatch() {
    stop_ = true;
    thread_.join();
    ::close(fd_);
  }
  std::set<std::string> touched() {
    std::lock_guard lock(mu_);
    return touched_;
  }
  std::set<std::string> jpegs() {
    std::lock_guard lock(mu_);
    return jpeg_;
  }

 private:
  int fd_;
  std::map<int, std::string> dirs_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mu_;
  std::set<std::string> touched_, jpeg_;
};

}  // namespace

class Collector : public ::testing::Test {
 protected:
  void SetUp() override {
    xvfb_ = std::make_unique<Xvfb>();
    if (!xvfb_->ok()) GTEST_SKIP() << "Xvfb not available";
    ASSERT_TRUE(backend_.start()) << "fake backend did not start";
    x_ = std::make_unique<XClient>(xvfb_->display());
    cfgDir_ = home_.str() + "/.config/thelife-collector";
    stateDir_ = home_.str() + "/.local/state/thelife-collector";
    log_ = home_.str() + "/collector.log";
  }
  void TearDown() override {
    if (daemon_) daemon_->stop(SIGKILL);
    if (HasFailure()) std::cerr << "---- collector log ----\n" << readFile(log_) << "-----------------------\n";
  }

  void enrolled(const std::string& extraToml = "") {
    std::string err;
    ASSERT_TRUE(files::ensurePrivateDir(cfgDir_, &err)) << err;
    ASSERT_TRUE(files::writePrivateAtomic(cfgDir_ + "/config.toml",
                                          "backend_url = \"" + backend_.url() + "\"\n" + extraToml, &err))
        << err;
    ASSERT_TRUE(files::writePrivateAtomic(cfgDir_ + "/device.token", std::string(kToken) + "\n", &err)) << err;
  }

  std::map<std::string, std::string> env() {
    return {{"HOME", home_.str()},
            {"DISPLAY", xvfb_->display()},
            {"XDG_CONFIG_HOME", ""},
            {"XDG_STATE_HOME", ""},
            {"THELIFE_COLLECTOR_FAST_TIMERS", "1"},
            {"THELIFE_COLLECTOR_NO_LOGIND", "1"},
            {"DBUS_SESSION_BUS_ADDRESS", busAddress_.empty() ? "unix:path=" + home_.str() + "/no-bus" : busAddress_},
            {"PATH", home_.str() + "/bin:" + std::string(std::getenv("PATH"))}};
  }

  void start() { daemon_ = std::make_unique<Child>(std::vector<std::string>{THELIFE_COLLECTOR_BIN, "run"}, env(), log_); }

  std::pair<int, std::string> cli(const std::vector<std::string>& args, const std::string& stdinText = "") {
    const auto out = home_.str() + "/cli.out";
    fs::remove(out);
    std::vector<std::string> argv{THELIFE_COLLECTOR_BIN};
    argv.insert(argv.end(), args.begin(), args.end());
    Child c(argv, env(), out, stdinText);
    const auto code = c.waitExit(30s);
    return {code.value_or(-1), readFile(out)};
  }

  std::vector<Json::Value> obsOf(const std::string& kind) {
    std::vector<Json::Value> out;
    for (const auto& o : backend_.observations()) {
      if (o["kind"].asString() == kind) out.push_back(o);
    }
    return out;
  }
  bool sawWindowTitle(const std::string& title) {
    for (const auto& o : obsOf("window")) {
      if (o["payload"]["title"].asString() == title) return true;
    }
    return false;
  }
  std::optional<Json::Value> captureMeta(const std::string& field, const std::string& value = "") {
    for (const auto& o : obsOf("capture_meta")) {
      const auto& p = o["payload"];
      if (p.isMember(field) && (value.empty() || p[field].asString() == value)) return p;
    }
    return std::nullopt;
  }

  TempDir home_;
  std::unique_ptr<Xvfb> xvfb_;
  FakeBackend backend_;
  std::unique_ptr<XClient> x_;
  std::unique_ptr<Child> daemon_;
  std::string cfgDir_, stateDir_, log_, busAddress_;
};

TEST_F(Collector, EnrollChecksTheTokenAndWritesPrivateFiles) {
  const std::string url = backend_.url();
  auto [code, out] = cli({"enroll", "--url", url, "--token", kToken});
  EXPECT_EQ(code, 64) << out;  // the token never goes on a command line
  std::tie(code, out) = cli({"enroll", "--url", "http://10.1.2.3:8080"}, std::string(kToken) + "\n");
  EXPECT_EQ(code, 64) << out;  // plain http off-host
  backend_.set(*json::parseStrict(R"({"tokens": ["tld_ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"]})"));
  std::tie(code, out) = cli({"enroll", "--url", url}, std::string(kToken) + "\n");
  EXPECT_EQ(code, 1) << out;
  EXPECT_NE(out.find("rejected the token"), std::string::npos) << out;
  EXPECT_FALSE(fs::exists(cfgDir_ + "/device.token"));

  backend_.set(*json::parseStrict(R"({"tokens": []})"));
  std::tie(code, out) = cli({"enroll", "--url", url, "--web-ui", "http://127.0.0.1:5999"}, std::string(kToken) + "\n");
  ASSERT_EQ(code, 0) << out;
  for (const auto* f : {"/config.toml", "/device.token"}) {
    struct stat st{};
    ASSERT_EQ(::stat((cfgDir_ + f).c_str(), &st), 0) << f;
    EXPECT_EQ(st.st_mode & 0777, 0600u) << f;
  }
  struct stat st{};
  ASSERT_EQ(::stat(cfgDir_.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700u);
  EXPECT_EQ(readFile(cfgDir_ + "/device.token"), std::string(kToken) + "\n");
  EXPECT_EQ(readFile(cfgDir_ + "/config.toml").find(kToken), std::string::npos);
  const auto gets = backend_.requests("GET", "/api/v1/devices/self/config");
  ASSERT_FALSE(gets.empty());
  EXPECT_EQ(gets.back()["token"].asString(), kToken);
}

TEST_F(Collector, RefusesAWorldReadableToken) {
  enrolled();
  ::chmod((cfgDir_ + "/device.token").c_str(), 0644);
  start();
  EXPECT_EQ(daemon_->waitExit(10s), 2);
  EXPECT_NE(readFile(log_).find("0600"), std::string::npos);
  EXPECT_TRUE(backend_.requests().empty());
}

TEST_F(Collector, ReportsActivityAndUploadsACaptureThatNeverTouchesDisk) {
  enrolled();
  const auto code = x_->createWindow(100, 100, 800, 600, 0x336699, "code", "main.cc — TheLife_v2",
                                     static_cast<uint32_t>(::getpid()));
  x_->activate(code);
  fs::create_directories(stateDir_);
  DiskWatch watch({home_.str(), cfgDir_, stateDir_, home_.str() + "/.local", home_.str() + "/.local/state", "/tmp",
                   fs::temp_directory_path().string()});
  start();

  ASSERT_TRUE(waitUntil([&] { return sawWindowTitle("main.cc — TheLife_v2"); }, 20s));
  ASSERT_TRUE(waitUntil([&] { return captureMeta("capture_id").has_value(); }, 30s));
  x_->setTitle(code, "util.cc — TheLife_v2");
  ASSERT_TRUE(waitUntil([&] { return sawWindowTitle("util.cc — TheLife_v2"); }, 15s));
  ASSERT_TRUE(waitUntil([&] { return backend_.requests("POST", "/api/v1/devices/self/heartbeat").size() >= 2; }, 15s));
  ASSERT_TRUE(waitUntil(
      [&] {
        for (const auto& r : backend_.requests("GET", "/api/v1/devices/self/config")) {
          if (r["headers"]["if-none-match"].asString() == "\"c1\"") return true;
        }
        return false;
      },
      15s));
  EXPECT_EQ(daemon_->stop(SIGTERM), 0);

  // Observations: each one valid against the backend's schema, sequences unique, the session closed.
  std::set<int64_t> seqs;
  std::set<std::string> kinds;
  for (const auto& o : backend_.observations()) {
    expectValid(o["kind"].asString(), o["payload"]);
    EXPECT_TRUE(parseRfc3339(o["captured_at"].asString())) << o["captured_at"];
    EXPECT_TRUE(seqs.insert(o["client_seq"].asInt64()).second) << "duplicate seq " << o["client_seq"];
    kinds.insert(o["kind"].asString());
  }
  EXPECT_TRUE(kinds.count("session") && kinds.count("window") && kinds.count("process_snapshot") &&
              kinds.count("capture_meta"));
  const auto sessions = obsOf("session");
  ASSERT_GE(sessions.size(), 2u);
  EXPECT_EQ(sessions.front()["payload"]["event"].asString(), "start");
  EXPECT_EQ(sessions.front()["payload"]["monitors"].size(), 2u);
  EXPECT_EQ(sessions.back()["payload"]["event"].asString(), "end");
  for (const auto& r : backend_.requests("POST", "/api/v1/ingest/observations")) {
    const auto key = r["headers"]["idempotency-key"].asString();
    const auto& batch = r["json"]["observations"];
    EXPECT_EQ(key.substr(key.find(':') + 1), std::to_string(batch[0]["client_seq"].asInt64()) + "-" +
                                                  std::to_string(batch[batch.size() - 1]["client_seq"].asInt64()));
    EXPECT_EQ(r["token"].asString(), kToken);
  }
  bool sawFull = false;
  for (const auto& o : obsOf("process_snapshot")) sawFull |= o["payload"]["full"].asBool();
  EXPECT_TRUE(sawFull);

  // The capture: a JSON context and one JPEG per monitor, scaled to capture_max_px.
  const auto caps = backend_.requests("POST", "/api/v1/ingest/captures");
  ASSERT_EQ(caps.size(), 1u);
  const auto& cap = caps[0];
  EXPECT_TRUE(cap["headers"]["idempotency-key"].asString().starts_with("cap-"));
  int images = 0;
  Json::Value ctx;
  for (const auto& p : cap["parts"]) {
    if (p["name"].asString() == "context") {
      EXPECT_EQ(p["content_type"].asString(), "application/json");
      ctx = *json::parseStrict(p["text"].asString());
    } else {
      EXPECT_EQ(p["name"].asString(), "images");
      EXPECT_EQ(p["content_type"].asString(), "image/jpeg");
      EXPECT_EQ(p["magic"].asString(), "ffd8ff");
      EXPECT_GT(p["size"].asInt(), 1000);
      ++images;
    }
  }
  EXPECT_EQ(images, 2);
  ASSERT_EQ(ctx["monitors"].size(), 2u);
  EXPECT_EQ(ctx["monitors"][0]["width"].asInt(), 1600);
  EXPECT_EQ(ctx["monitors"][0]["height"].asInt(), 900);
  EXPECT_EQ(ctx["active_monitor"].asInt(), 0);
  EXPECT_EQ(ctx["window"]["title"].asString(), "main.cc — TheLife_v2");
  EXPECT_TRUE(parseRfc3339(ctx["captured_at"].asString()));
  const auto meta = captureMeta("capture_id");
  EXPECT_EQ((*meta)["monitors"].asInt(), 2);

  const auto hb = backend_.requests("POST", "/api/v1/devices/self/heartbeat").back()["json"];
  EXPECT_EQ(hb["version"].asString(), THELIFE_COLLECTOR_VERSION);
  EXPECT_TRUE(hb["x_ok"].asBool());
  EXPECT_TRUE(hb["queue_depth"].isIntegral());
  EXPECT_TRUE(hb["last_capture_at"].isString());

  // Nothing but the queue and the state file was written, and no JPEG bytes reached any file.
  EXPECT_TRUE(watch.jpegs().empty()) << *watch.jpegs().begin();
  for (const auto& p : watch.touched()) {
    if (p.starts_with(stateDir_ + "/")) {
      const auto name = p.substr(stateDir_.size() + 1);
      EXPECT_TRUE(name.starts_with("queue.db") || name.starts_with("state.json")) << p;
    }
    EXPECT_FALSE(p.starts_with(cfgDir_ + "/")) << p;
  }
  for (const auto& e : fs::directory_iterator(stateDir_)) {
    EXPECT_FALSE(hasJpegMagic(readFile(e.path().string()))) << e.path();
  }
}

TEST_F(Collector, QueuesWhileOfflineThenDrainsWithoutDuplicates) {
  enrolled();
  x_->activate(x_->createWindow(0, 0, 400, 300, 0x00ff00, "term", "first run", std::nullopt));
  start();
  ASSERT_TRUE(waitUntil([&] { return captureMeta("capture_id").has_value(); }, 30s));
  EXPECT_EQ(daemon_->stop(SIGTERM), 0);
  const auto firstRun = backend_.observations().size();

  backend_.stop();
  x_->activate(x_->createWindow(0, 0, 400, 300, 0x00ff00, "term", "offline run", std::nullopt));
  start();  // the cached config says capture is on, but the backend is unreachable
  ASSERT_TRUE(waitUntil(
      [&] {
        const auto [code, out] = cli({"status"});
        return out.find("unreachable") != std::string::npos && out.find("queued:           0 ") == std::string::npos;
      },
      20s));
  std::this_thread::sleep_for(5s);  // the first capture (due 2 s after start) comes while the backend is down
  ASSERT_TRUE(backend_.start());     // same port; it has forgotten the first run's sequences
  ASSERT_TRUE(waitUntil([&] { return captureMeta("skipped", "offline").has_value(); }, 60s)) << [&] {
    std::string all;
    for (const auto& o : backend_.observations()) all += o["kind"].asString() + " " + json::compact(o["payload"]) + "\n";
    return all;
  }();
  ASSERT_TRUE(waitUntil([&] { return sawWindowTitle("offline run"); }, 30s));
  ASSERT_TRUE(waitUntil([&] { return cli({"status"}).second.find("queued:           0 ") != std::string::npos; }, 30s));
  std::set<int64_t> seqs;
  for (const auto& o : backend_.observations()) EXPECT_TRUE(seqs.insert(o["client_seq"].asInt64()).second);
  EXPECT_GT(firstRun, 0u);
  // While it was down, the capture was dropped rather than kept for later.
  EXPECT_TRUE(backend_.requests("POST", "/api/v1/ingest/captures").empty());
}

TEST_F(Collector, CaptureRefusalsAreRecordedAndAStaleConfigIsRefetched) {
  enrolled();
  x_->activate(x_->createWindow(0, 0, 400, 300, 0x00ff00, "term", "t", std::nullopt));
  backend_.set(*json::parseStrict(R"({"overrides": {"POST /api/v1/ingest/captures":
      {"status": 409, "body": "{\"title\": \"Capture is off\"}"}}})"));
  start();
  ASSERT_TRUE(waitUntil([&] { return captureMeta("skipped", "disabled").has_value(); }, 30s));
  const auto capAt = backend_.requests("POST", "/api/v1/ingest/captures").front()["at"].asDouble();
  bool refetched = false;
  for (const auto& r : backend_.requests("GET", "/api/v1/devices/self/config")) {
    const auto at = r["at"].asDouble();
    refetched |= at > capAt && at < capAt + 1.5;  // at once, not at the next 3 s poll
  }
  EXPECT_TRUE(refetched);
  daemon_->stop(SIGTERM);

  backend_.set(*json::parseStrict(R"({"overrides": {"POST /api/v1/ingest/captures":
      {"status": 503, "body": "{}", "headers": {"Retry-After": "60"}}}})"));
  start();
  ASSERT_TRUE(waitUntil([&] { return captureMeta("skipped", "backend_busy").has_value(); }, 30s));
}

TEST_F(Collector, AHostileBackendCanOnlyChangeClampedSettings) {
  enrolled();
  // Out-of-range values and fields that look like commands: clamped and ignored.
  Json::Value s;
  s["config_raw"] = R"({"enabled": true, "capture_interval_s": 1, "jitter_pct": 0, "capture_monitors": "all",
      "capture_max_px": 100000, "idle_threshold_s": 1, "process_interval_s": 1, "command": "rm -rf ~",
      "upload_url": "https://evil.example/", "capture_now": true})";
  backend_.set(s);
  x_->activate(x_->createWindow(0, 0, 400, 300, 0x00ff00, "term", "t", std::nullopt));
  start();
  ASSERT_TRUE(waitUntil([&] { return readFile(log_).find("clamped capture_interval_s 1 -> 60") != std::string::npos; },
                        20s));
  auto log = readFile(log_);
  EXPECT_NE(log.find("clamped capture_max_px 100000 -> 2560"), std::string::npos);
  EXPECT_NE(log.find("clamped idle_threshold_s 1 -> 30"), std::string::npos);
  EXPECT_NE(log.find("clamped process_interval_s 1 -> 15"), std::string::npos);
  // The first capture comes 2 s after start, and no second one within the clamped 60 s minimum.
  ASSERT_TRUE(waitUntil([&] { return !backend_.requests("POST", "/api/v1/ingest/captures").empty(); }, 20s));
  std::this_thread::sleep_for(8s);
  EXPECT_EQ(backend_.requests("POST", "/api/v1/ingest/captures").size(), 1u);

  // A redirect is not followed.
  s = Json::Value();
  s["redirect_config"] = true;
  s["etag"] = "\"c2\"";
  backend_.set(s);
  backend_.clear();
  ASSERT_TRUE(waitUntil([&] { return backend_.requests("GET", "/api/v1/devices/self/config").size() >= 2; }, 20s));
  EXPECT_TRUE(backend_.requests("GET", "/api/v1/evil").empty());

  // Deeply nested JSON is rejected, and the clamped config stays in force.
  std::string deep = R"({"enabled": false, "x": )";
  for (int i = 0; i < 40; ++i) deep += "[";
  for (int i = 0; i < 40; ++i) deep += "]";
  deep += "}";
  s["redirect_config"] = false;
  s["config_raw"] = deep;
  s["etag"] = "\"c3\"";
  backend_.set(s);
  ASSERT_TRUE(waitUntil([&] { return readFile(log_).find("config: rejected") != std::string::npos; }, 20s));
  EXPECT_EQ(readFile(log_).find("enabled: true -> false"), std::string::npos);
  for (const auto& r : backend_.requests()) {
    const auto p = r["path"].asString();
    EXPECT_TRUE(p.starts_with("/api/v1/devices/self/") || p.starts_with("/api/v1/ingest/")) << p;
  }
}

TEST_F(Collector, PauseStopsObservingUntilResumed) {
  enrolled();
  const auto a = x_->createWindow(0, 0, 400, 300, 0x00ff00, "term", "before pause", std::nullopt);
  x_->activate(a);
  start();
  ASSERT_TRUE(waitUntil([&] { return sawWindowTitle("before pause"); }, 20s));
  auto [code, out] = cli({"pause", "10m"});
  ASSERT_EQ(code, 0) << out;
  EXPECT_NE(out.find("Paused for 10m"), std::string::npos) << out;
  std::this_thread::sleep_for(3s);  // the daemon checks every 2 s
  const auto b = x_->createWindow(0, 0, 400, 300, 0x0000ff, "browser", "Secret plans", std::nullopt);
  x_->activate(b);
  std::this_thread::sleep_for(4s);
  EXPECT_FALSE(sawWindowTitle("Secret plans"));
  EXPECT_NE(cli({"status"}).second.find("paused until"), std::string::npos);
  ASSERT_TRUE(waitUntil(
      [&] { return !backend_.requests("POST", "/api/v1/devices/self/heartbeat").back()["json"]["paused_until"].isNull(); },
      10s));
  std::tie(code, out) = cli({"resume"});
  ASSERT_EQ(code, 0) << out;
  ASSERT_TRUE(waitUntil([&] { return sawWindowTitle("Secret plans"); }, 15s));  // reported again once resumed
  std::tie(code, out) = cli({"pause", "forever"});
  EXPECT_EQ(code, 64);
}

TEST_F(Collector, RotatesAnOldTokenAndUsesTheNewOne) {
  enrolled();
  std::string err;
  ASSERT_TRUE(files::ensurePrivateDir(stateDir_, &err));
  {
    Queue q(stateDir_ + "/queue.db");
    q.setMeta(kMetaTokenIssuedAt, rfc3339(std::chrono::system_clock::now() - std::chrono::hours(24 * 31)));
  }
  const std::string fresh = "tld_" + std::string(43, 'N');
  Json::Value s;
  s["tokens"].append(kToken);
  s["rotate_to"] = fresh;
  backend_.set(s);
  start();
  ASSERT_TRUE(waitUntil([&] { return readFile(cfgDir_ + "/device.token") == fresh + "\n"; }, 20s));
  struct stat st{};
  ::stat((cfgDir_ + "/device.token").c_str(), &st);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  backend_.clear();
  ASSERT_TRUE(waitUntil([&] { return !backend_.requests("POST", "/api/v1/devices/self/heartbeat").empty(); }, 10s));
  for (const auto& r : backend_.requests()) EXPECT_EQ(r["token"].asString(), fresh);
  EXPECT_EQ(readFile(log_).find(fresh), std::string::npos);  // never logged
  EXPECT_EQ(readFile(log_).find(kToken), std::string::npos);
}

TEST_F(Collector, ARevokedTokenStopsUploadsWithoutAStormUntilReEnrolled) {
  enrolled();
  Json::Value s;
  s["tokens"].append(kOtherToken());
  backend_.set(s);
  start();
  std::this_thread::sleep_for(8s);
  EXPECT_LT(backend_.requests().size(), 10u);
  EXPECT_NE(readFile(log_).find("device token was refused"), std::string::npos);
  EXPECT_NE(cli({"status"}).second.find("token refused"), std::string::npos);
  // Re-enrolling writes a new token file; the running daemon picks it up.
  std::string err;
  ASSERT_TRUE(files::writePrivateAtomic(cfgDir_ + "/device.token", kOtherToken() + "\n", &err));
  ASSERT_TRUE(waitUntil([&] { return !backend_.observations().empty(); }, 20s));
}

// ---- desktop notifications over a private session bus ----

namespace {

struct NotifyServer {
  sd_bus* bus = nullptr;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::vector<Json::Value> calls;
  std::optional<uint32_t> clickRequest;
  uint32_t nextId = 1;

  static int onNotify(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<NotifyServer*>(ud);
    const char *app, *icon, *summary, *body;
    uint32_t replaces;
    Json::Value call;
    sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
    call["app"] = app;
    call["summary"] = summary;
    call["body"] = body;
    char** actions = nullptr;
    sd_bus_message_read_strv(m, &actions);
    for (char** a = actions; a && *a; ++a) {
      call["actions"].append(*a);
      free(*a);
    }
    free(actions);
    sd_bus_message_enter_container(m, 'a', "{sv}");
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
      const char* key;
      sd_bus_message_read(m, "s", &key);
      if (std::string(key) == "urgency") {
        uint8_t u;
        sd_bus_message_read(m, "v", "y", &u);
        call["urgency"] = u;
      } else {
        sd_bus_message_skip(m, "v");
      }
      sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    std::lock_guard lock(self->mu);
    const uint32_t id = self->nextId++;
    call["id"] = id;
    self->calls.push_back(call);
    return sd_bus_reply_method_return(m, "u", id);
  }

  bool start(const std::string& address) {
    if (sd_bus_new(&bus) < 0 || sd_bus_set_address(bus, address.c_str()) < 0 || sd_bus_set_bus_client(bus, 1) < 0 ||
        sd_bus_start(bus) < 0) {
      return false;
    }
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", onNotify, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_SIGNAL("ActionInvoked", "us", 0),
        SD_BUS_VTABLE_END};
    if (sd_bus_add_object_vtable(bus, nullptr, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                 vtable, this) < 0 ||
        sd_bus_request_name(bus, "org.freedesktop.Notifications", 0) < 0) {
      return false;
    }
    thread = std::thread([this] {
      while (!stop) {
        while (sd_bus_process(bus, nullptr) > 0) {
        }
        std::optional<uint32_t> click;
        {
          std::lock_guard lock(mu);
          click.swap(clickRequest);
        }
        if (click) {
          sd_bus_emit_signal(bus, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked",
                             "us", *click, "default");
        }
        sd_bus_wait(bus, 100'000);
      }
    });
    return true;
  }
  ~NotifyServer() {
    stop = true;
    if (thread.joinable()) thread.join();
    sd_bus_flush_close_unref(bus);
  }
};

}  // namespace

TEST_F(Collector, ShowsNotificationsAsPlainTextAndOpensOnlyLocalUrls) {
  const auto socket = home_.str() + "/bus";
  busAddress_ = "unix:path=" + socket;
  Child dbus({"dbus-daemon", "--session", "--nofork", "--nopidfile", "--address=" + busAddress_}, {},
             home_.str() + "/dbus.log");
  ASSERT_TRUE(waitUntil([&] { return fs::exists(socket); }, 10s)) << "dbus-daemon did not start";
  NotifyServer server;
  ASSERT_TRUE(server.start(busAddress_));
  // A fake browser launcher records what the collector asks it to open.
  fs::create_directories(home_.str() + "/bin");
  {
    std::ofstream f(home_.str() + "/bin/xdg-open");
    f << "#!/bin/sh\necho \"$@\" >> \"" << home_.str() << "/opened\"\n";
  }
  fs::permissions(home_.str() + "/bin/xdg-open", fs::perms::owner_all);

  enrolled("web_ui_url = \"http://127.0.0.1:5999\"\n");
  const char* nid = "0196f1c2-7a3b-7c4d-8e5f-0123456789ab";
  backend_.set(*json::parseStrict(std::string(R"({"notifications": [{"id": ")") + nid +
                                  R"(", "title": "<b>Stand up</b>", "body": "2h & counting <a href='x'>",
      "urgency": "critical", "action": "open_chat", "ref_id": null, "url": "https://evil.example/"}]})"));
  x_->fakeMotion(5, 5);  // present, so nothing is held
  start();
  ASSERT_TRUE(waitUntil(
      [&] {
        std::lock_guard lock(server.mu);
        return !server.calls.empty();
      },
      20s));
  Json::Value call;
  {
    std::lock_guard lock(server.mu);
    call = server.calls[0];
  }
  EXPECT_EQ(call["app"].asString(), "TheLife");
  EXPECT_EQ(call["summary"].asString(), "&lt;b&gt;Stand up&lt;/b&gt;");
  EXPECT_EQ(call["body"].asString(), "2h &amp; counting &lt;a href=&#39;x&#39;&gt;");
  EXPECT_EQ(call["urgency"].asInt(), 2);
  ASSERT_EQ(call["actions"].size(), 2u);
  EXPECT_EQ(call["actions"][0].asString(), "default");
  ASSERT_TRUE(waitUntil([&] { return backend_.state()["acked"].size() >= 1; }, 10s));

  {
    std::lock_guard lock(server.mu);
    server.clickRequest = call["id"].asUInt();
  }
  ASSERT_TRUE(waitUntil([&] { return fs::exists(home_.str() + "/opened"); }, 10s));
  EXPECT_EQ(readFile(home_.str() + "/opened"), "http://127.0.0.1:5999/chat\n");
  ASSERT_TRUE(waitUntil(
      [&] {
        for (const auto& r : backend_.requests()) {
          if (r["path"].asString().ends_with(":ack") && r["json"]["clicked"].asBool()) return true;
        }
        return false;
      },
      10s));
  // Shown once, even though the backend served it on more than one poll before the ack.
  std::this_thread::sleep_for(2s);
  std::lock_guard lock(server.mu);
  EXPECT_EQ(server.calls.size(), 1u);
}
