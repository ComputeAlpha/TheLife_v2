#include "app/daemon.h"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "app/state.h"
#include "capture/image.h"
#include "config/server_config.h"
#include "core/json.h"
#include "core/log.h"
#include "net/backoff.h"
#include "net/http.h"
#include "net/uploader.h"
#include "notify/desktop.h"
#include "notify/notifications.h"
#include "probes/idle_probe.h"
#include "probes/lock_probe.h"
#include "probes/proc_probe.h"
#include "probes/window_probe.h"
#include "queue/queue.h"
#include "sched/scheduler.h"
#include "x11/xcb_source.h"

extern char** environ;

namespace thelife::collector {

Timing Timing::fromEnv() {
  Timing t;
  if (const char* f = std::getenv("THELIFE_COLLECTOR_FAST_TIMERS"); f && std::string(f) == "1") {
    t.firstCapture = Seconds(2);
    t.configEvery = Seconds(3);
    t.heartbeatEvery = Seconds(2);
    t.uploadDelay = Seconds(1);
    t.stateEvery = Seconds(1);
    t.capCheckEvery = Seconds(5);
    t.focusSettle = Seconds(2);
    t.tokenReloadEvery = Seconds(2);
    t.backoffCap = Seconds(3);
  }
  return t;
}

namespace {

std::atomic<bool> gStop{false};
extern "C" void onSignal(int) { gStop = true; }

constexpr int kJpegQuality = 80;
constexpr auto kQueueMaxAge = std::chrono::hours(24 * 7);
constexpr int64_t kQueueMaxRows = 200'000;

SysTime now() { return std::chrono::system_clock::now(); }

uint64_t randomSeed() {
  std::random_device rd;
  return (static_cast<uint64_t>(rd()) << 32) ^ rd();
}

std::string randomHex(size_t bytes) {
  std::random_device rd;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < bytes; ++i) {
    const auto b = static_cast<unsigned>(rd() & 0xFF);
    out += kHex[b >> 4];
    out += kHex[b & 0xF];
  }
  return out;
}

void sleepUnlessStopped(Seconds d) {
  const auto until = std::chrono::steady_clock::now() + d;
  while (!gStop && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

struct CaptureJob {
  SysTime capturedAt;
  std::vector<Monitor> monitors;
  int activeMonitor = 0;
  Json::Value window;
  uint32_t idleMs = 0;
  Json::Value processes;
  int maxPx = 1600;
};

class Daemon {
 public:
  Daemon(const LocalConfig& cfg, TokenStore& token, const DaemonOptions& opt)
      : cfg_(cfg), token_(token), opt_(opt), ua_(std::string("thelife-collector/") + THELIFE_COLLECTOR_VERSION) {}

  int run();

 private:
  std::unique_ptr<CurlHttp> http(bool abortOnStop) {
    auto h = std::make_unique<CurlHttp>(cfg_, [this] { return token_.get(); }, ua_);
    if (abortOnStop) h->setAbort([] { return gStop.load(); });
    return h;
  }
  void setError(const std::string& e) {
    std::lock_guard lock(mu_);
    lastError_ = e;
  }
  void clearError() {
    std::lock_guard lock(mu_);
    lastError_.clear();
  }
  void captureMeta(SysTime at, const char* skipped) {
    Json::Value p;
    p["skipped"] = skipped;
    queue_->push("capture_meta", at, p);
  }
  void loadCachedConfig();
  void session(const char* event, const std::vector<Monitor>& ms);

  int mainLoop(XcbSource& xs);
  void captureLoop();
  void netLoop();
  void pollerLoop();
  void busLoop(std::unique_ptr<DesktopNotifier> dn);

  Seconds fetchConfig(IHttp& h, Backoff& b);
  void heartbeat(IHttp& h);
  void maybeRotate(IHttp& h);
  void writeStateFile();
  void ack(IHttp& h, const Note& n, bool clicked);

  const LocalConfig& cfg_;
  TokenStore& token_;
  DaemonOptions opt_;
  std::string ua_;
  std::unique_ptr<Queue> queue_;
  std::string installId_;

  // Shared between threads.
  std::mutex mu_;
  ServerConfig config_;
  bool configKnown_ = false;
  std::string etag_, lastError_;
  std::optional<SysTime> lastCaptureAt_, lastUploadAt_;
  std::optional<SysTime> pausedUntil_;
  std::atomic<bool> online_{false}, unauthorized_{false}, configStale_{false}, xOk_{true}, userPresent_{true};
  std::atomic<bool> paused_{false};

  std::mutex netMu_;
  std::condition_variable netCv_;

  std::mutex capMu_;
  std::condition_variable capCv_;
  std::optional<CaptureJob> job_;
  std::atomic<bool> capturing_{false};

  std::mutex noteMu_;
  NoteHolder holder_;
};

void Daemon::loadCachedConfig() {
  const auto cached = queue_->meta(kMetaServerConfig);
  if (!cached) return;
  const auto doc = json::parseStrict(*cached);
  std::string err;
  if (const auto p = doc ? parseServerConfig(*doc, &err) : std::nullopt) {
    std::lock_guard lock(mu_);
    config_ = p->config;
    configKnown_ = true;
    etag_ = queue_->meta(kMetaConfigEtag).value_or("");
  }
}

void Daemon::session(const char* event, const std::vector<Monitor>& ms) {
  Json::Value p;
  p["event"] = event;
  p["collector_version"] = THELIFE_COLLECTOR_VERSION;
  p["x_screen"] = json::utf8Truncate(opt_.display.empty() ? (std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "")
                                                          : opt_.display,
                                     64);
  auto& arr = p["monitors"] = Json::Value(Json::arrayValue);
  for (const auto& m : ms) {
    Json::Value j;
    j["index"] = m.index;
    j["x"] = m.r.x;
    j["y"] = m.r.y;
    j["width"] = m.r.w;
    j["height"] = m.r.h;
    j["primary"] = m.primary;
    arr.append(j);
  }
  queue_->push("session", now(), p);
}

int Daemon::run() {
  std::string err;
  auto xs = XcbSource::connect(opt_.display.empty() ? nullptr : opt_.display.c_str(), &err);
  if (!xs) {
    log::error("x11: {}", err);
    return kExitXLost;
  }
  try {
    queue_ = std::make_unique<Queue>(queuePath());
  } catch (const std::exception& e) {
    log::error("{}", e.what());
    return kExitConfig;
  }
  installId_ = queue_->meta(kMetaInstallId).value_or("");
  if (installId_.empty()) {
    installId_ = randomHex(8);
    queue_->setMeta(kMetaInstallId, installId_);
  }
  if (!queue_->meta(kMetaTokenIssuedAt)) queue_->setMeta(kMetaTokenIssuedAt, rfc3339(now()));
  loadCachedConfig();

  std::unique_ptr<DesktopNotifier> dn = DesktopNotifier::open(&err);
  if (!dn) log::warn("notifications: off ({}); the backend falls back to Telegram", err);

  std::thread net([this] { netLoop(); });
  std::thread cap([this] { captureLoop(); });
  std::thread poller, bus;
  if (dn) {
    poller = std::thread([this] { pollerLoop(); });
    bus = std::thread([this, d = std::move(dn)]() mutable { busLoop(std::move(d)); });
  }

  log::info("collector {} started (display {}, backend {})", THELIFE_COLLECTOR_VERSION, xs->screenName(),
            cfg_.backendUrl);
  const int code = mainLoop(*xs);

  gStop = true;
  capCv_.notify_all();
  netCv_.notify_all();
  cap.join();
  net.join();
  if (poller.joinable()) poller.join();
  if (bus.joinable()) bus.join();
  writeStateFile();
  log::info("collector stopped (exit {})", code);
  return code;
}

int Daemon::mainLoop(XcbSource& xs) {
  const auto& t = opt_.timing;
  WindowProbe wp(xs, opt_.procRoot, [this](SysTime at, const Json::Value& p) { queue_->push("window", at, p); });
  IdleProbe ip([this](SysTime at, const Json::Value& p) { queue_->push("idle", at, p); });
  ProcProbe pp(opt_.procRoot, ::getuid(), cfg_.argsAllowlist);
  LockProbe lp;
  Scheduler sched(randomSeed(), t.firstCapture);

  auto monitors = xs.monitors();
  wp.setMonitors(monitors);
  session("start", monitors);
  xs.watch(wp.refresh(now(), true));

  SysTime start = now();
  SysTime nextIdle = start, nextProc = start, nextPauseCheck = start, nextCap = start + t.capCheckEvery;
  uint32_t lastIdleMs = 0;
  std::string focusApp, appAtCapture;
  SysTime focusSince = start;
  bool focusTriggered = true;
  pollfd pfd{xs.fd(), POLLIN, 0};

  while (!gStop) {
    ::poll(&pfd, 1, 250);
    const auto ev = xs.drain();
    if (ev.lost) {
      xOk_ = false;
      log::error("x11: connection lost");
      return kExitXLost;
    }
    const auto n = now();

    if (n >= nextPauseCheck) {
      std::optional<SysTime> until;
      if (const auto s = queue_->meta(kMetaPausedUntil)) until = parseRfc3339(*s);
      const bool p = until && *until > n;
      if (p != paused_) {
        if (p) log::info("paused until {}", rfc3339(*until));
        else log::info("pause over");
        if (!p) {
          wp.forget();  // report the focused window again
          xs.watch(wp.refresh(n, true));
        }
      }
      paused_ = p;
      {
        std::lock_guard lock(mu_);
        pausedUntil_ = p ? until : std::nullopt;
      }
      nextPauseCheck = n + Seconds(2);
    }

    ServerConfig cfg;
    bool known;
    {
      std::lock_guard lock(mu_);
      cfg = config_;
      known = configKnown_;
    }

    if (!paused_) {
      if (ev.activeChanged) xs.watch(wp.refresh(n, true));
      else if (ev.windowChanged) wp.refresh(n, false);
      wp.tick(n);

      if (n >= nextIdle) {
        const auto info = xs.idle();
        const auto app = wp.appKey();
        const bool locked = info.screensaverOn || (opt_.useLogind ? lp.locked(app) : LockProbe::isLockerClass(app));
        ip.sample(n, info.msSinceInput, locked, wp.fullscreen(), cfg.idleThresholdS);
        lastIdleMs = info.msSinceInput;
        userPresent_ = !ip.idle();
        nextIdle = n + IdleProbe::kInterval;
      }
      if (n >= nextProc) {
        monitors = xs.monitors();
        wp.setMonitors(monitors);
        queue_->push("process_snapshot", n, pp.snapshot(n, wp.pid(), xs.clientPids()));
        nextProc = n + Seconds(cfg.processIntervalS);
      }
      // Context-change trigger: a new app that stays focused may pull the next capture forward (bounded by the
      // scheduler to one per interval).
      if (const auto app = wp.appKey(); app != focusApp) {
        focusApp = app;
        focusSince = n;
        focusTriggered = false;
      }
      if (!focusTriggered && !focusApp.empty() && n - focusSince >= t.focusSettle && focusApp != appAtCapture) {
        focusTriggered = true;
        sched.pullForward(n, cfg.captureIntervalS);
      }
    }

    SchedState s;
    s.configKnown = known;
    s.enabled = cfg.enabled;
    s.idle = ip.idle();
    s.passive = ip.passive();
    s.locked = ip.locked();
    s.paused = paused_;
    s.online = online_ && !unauthorized_;
    s.intervalS = cfg.captureIntervalS;
    s.jitterPct = cfg.jitterPct;
    const auto d = sched.tick(n, s);
    if (d.kind == SchedDecision::Kind::Skip) {
      captureMeta(n, d.skipReason.c_str());
    } else if (d.kind == SchedDecision::Kind::Capture) {
      appAtCapture = wp.appKey();
      CaptureJob job;
      job.capturedAt = n;
      const auto active = wp.monitor();
      job.activeMonitor = active.value_or(0);
      for (const auto& m : monitors) {
        if (cfg.captureMonitors == "all" || m.index == job.activeMonitor) job.monitors.push_back(m);
      }
      if (job.monitors.empty() && !monitors.empty()) job.monitors.push_back(monitors.front());
      job.window = wp.current();
      job.idleMs = lastIdleMs;
      job.processes = Json::Value(Json::arrayValue);
      for (const auto& p : pp.top(15)) {
        Json::Value j;
        j["exe"] = json::utf8Truncate(p.exe, 4096);
        j["comm"] = json::utf8Truncate(p.comm, 64);
        j["cpu_pct"] = p.cpuPct;
        j["rss_mb"] = p.rssMb;
        j["focused"] = p.focused;
        job.processes.append(j);
      }
      job.maxPx = cfg.captureMaxPx;
      std::lock_guard lock(capMu_);
      if (capturing_ || job_) {
        captureMeta(n, "error");  // the previous capture is still uploading
      } else {
        job_ = std::move(job);
        capCv_.notify_one();
      }
    }

    if (n >= nextCap) {
      if (const auto dropped = queue_->enforceCap(n, kQueueMaxAge, kQueueMaxRows); dropped > 0) {
        log::warn("queue: dropped {} old observation(s); a gap was recorded", dropped);
      }
      nextCap = n + t.capCheckEvery;
    }
  }
  session("end", monitors);
  return kExitOk;
}

void Daemon::captureLoop() {
  std::string err;
  // A connection of its own: grabbing never blocks the event loop.
  auto xs = XcbSource::connect(opt_.display.empty() ? nullptr : opt_.display.c_str(), &err);
  if (!xs) log::error("capture: {}", err);
  auto h = http(true);
  for (;;) {
    CaptureJob job;
    {
      std::unique_lock lock(capMu_);
      capCv_.wait(lock, [this] { return gStop || job_.has_value(); });
      if (gStop) return;
      job = std::move(*job_);
      job_.reset();
      capturing_ = true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<SecureBytes> jpegs;
    Json::Value mons(Json::arrayValue);
    bool failed = !xs;
    for (const auto& m : job.monitors) {
      if (failed) break;
      auto img = xs->grab(m.r, &err);
      if (!img) {
        failed = true;
        break;
      }
      Image scaled = downscale(std::move(*img), job.maxPx);
      auto jpeg = encodeJpeg(scaled, kJpegQuality, &err);
      Json::Value j;
      j["index"] = m.index;
      j["width"] = scaled.width;
      j["height"] = scaled.height;
      scaled.px.wipe();
      if (!jpeg) {
        failed = true;
        break;
      }
      mons.append(j);
      jpegs.push_back(std::move(*jpeg));
    }
    if (failed || jpegs.empty()) {
      log::warn("capture: {}", err.empty() ? "no monitor captured" : err);
      jpegs.clear();
      captureMeta(job.capturedAt, "error");
      capturing_ = false;
      continue;
    }

    Json::Value ctx;
    ctx["captured_at"] = rfc3339(job.capturedAt);
    ctx["monitors"] = mons;
    ctx["active_monitor"] = job.activeMonitor;
    ctx["window"] = job.window.isObject() ? job.window : Json::Value();
    ctx["idle_ms"] = job.idleMs;
    ctx["processes"] = job.processes;
    HttpRequest req;
    req.method = "POST";
    req.path = "/ingest/captures";
    req.timeoutS = 30;
    req.headers.emplace_back("Idempotency-Key", "cap-" + randomHex(12));
    req.parts.push_back({"context", "", "application/json", json::compact(ctx), nullptr, 0});
    for (size_t i = 0; i < jpegs.size(); ++i) {
      req.parts.push_back({"images", "monitor-" + std::to_string(i) + ".jpg", "image/jpeg", "", jpegs[i].data(),
                           jpegs[i].size()});
    }
    const auto res = h->send(req);
    jpegs.clear();  // zeroed whether the upload worked or not; a capture is never retried or kept
    req.parts.clear();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    if (res.status == 202 || res.status == 200 || res.status == 201) {
      const auto doc = json::parseStrict(res.body);
      Json::Value meta;
      if (doc && (*doc)["capture_id"].isString() && isUuid((*doc)["capture_id"].asString())) {
        meta["capture_id"] = (*doc)["capture_id"];
      }
      meta["monitors"] = static_cast<int>(mons.size());
      meta["active_monitor"] = job.activeMonitor;
      meta["duration_ms"] = static_cast<Json::Int64>(ms.count());
      queue_->push("capture_meta", job.capturedAt, meta);
      online_ = true;
      std::lock_guard lock(mu_);
      lastCaptureAt_ = job.capturedAt;
    } else if (!res.reached()) {
      online_ = false;
      captureMeta(job.capturedAt, "offline");
    } else if (res.status == 503 || res.status == 429) {
      captureMeta(job.capturedAt, "backend_busy");
    } else if (res.status == 409) {
      captureMeta(job.capturedAt, "disabled");
      configStale_ = true;
      netCv_.notify_all();
    } else if (res.status == 401) {
      unauthorized_ = true;
      captureMeta(job.capturedAt, "error");
    } else {
      log::warn("capture: upload refused with HTTP {}", res.status);
      captureMeta(job.capturedAt, "error");
    }
    capturing_ = false;
  }
}

Seconds Daemon::fetchConfig(IHttp& h, Backoff& b) {
  HttpRequest req;
  req.path = "/devices/self/config";
  req.timeoutS = 20;
  {
    std::lock_guard lock(mu_);
    if (configKnown_ && !etag_.empty()) req.headers.emplace_back("If-None-Match", etag_);
  }
  const auto res = h.send(req);
  if (!res.reached()) {
    online_ = false;
    setError("backend unreachable: " + res.error);
    return b.next();
  }
  online_ = true;
  b.reset();
  if (res.status == 304) return opt_.timing.configEvery;
  if (res.status == 401) {
    unauthorized_ = true;
    return opt_.timing.configEvery;
  }
  if (res.status != 200) {
    setError("config: HTTP " + std::to_string(res.status));
    return opt_.timing.configEvery;
  }
  std::string err = "not JSON";
  const auto doc = json::parseStrict(res.body);
  const auto parsed = doc ? parseServerConfig(*doc, &err) : std::nullopt;
  if (!parsed) {
    log::warn("config: rejected ({}); keeping the current one", err);
    setError("config rejected: " + err);
    return opt_.timing.configEvery;
  }
  for (const auto& c : parsed->clamped) log::warn("config: clamped {}", c);
  std::string etag;
  if (const auto it = res.headers.find("etag"); it != res.headers.end() && it->second.size() <= 200) etag = it->second;
  ServerConfig old;
  bool known;
  {
    std::lock_guard lock(mu_);
    old = config_;
    known = configKnown_;
    config_ = parsed->config;
    configKnown_ = true;
    etag_ = etag;
  }
  // Local audit log of every applied change (collector_lld.md §8).
  if (!known) {
    log::info("config: applied {}", json::compact(toJson(parsed->config)));
  } else {
    for (const auto& c : describeChanges(old, parsed->config)) log::info("config: {}", c);
  }
  queue_->setMeta(kMetaServerConfig, json::compact(toJson(parsed->config)));
  queue_->setMeta(kMetaConfigEtag, etag);
  return opt_.timing.configEvery;
}

void Daemon::heartbeat(IHttp& h) {
  Json::Value body;
  body["version"] = THELIFE_COLLECTOR_VERSION;
  body["queue_depth"] = static_cast<Json::Int64>(queue_->depth());
  body["x_ok"] = xOk_.load();
  {
    std::lock_guard lock(mu_);
    body["paused_until"] = pausedUntil_ ? Json::Value(rfc3339(*pausedUntil_)) : Json::Value();
    body["last_capture_at"] = lastCaptureAt_ ? Json::Value(rfc3339(*lastCaptureAt_)) : Json::Value();
    body["last_error"] = lastError_.empty() ? Json::Value() : Json::Value(json::utf8Truncate(lastError_, 500));
  }
  HttpRequest req;
  req.method = "POST";
  req.path = "/devices/self/heartbeat";
  req.timeoutS = 20;
  req.body = json::compact(body);
  const auto res = h.send(req);
  if (!res.reached()) {
    online_ = false;
    return;
  }
  online_ = true;
  if (res.status == 401) unauthorized_ = true;
}

void Daemon::maybeRotate(IHttp& h) {
  const auto issued = queue_->meta(kMetaTokenIssuedAt);
  const auto at = issued ? parseRfc3339(*issued) : std::nullopt;
  if (!at) {
    queue_->setMeta(kMetaTokenIssuedAt, rfc3339(now()));
    return;
  }
  if (now() - *at < opt_.timing.tokenMaxAge) return;
  HttpRequest req;
  req.method = "POST";
  req.path = "/devices/self/token:rotate";
  req.timeoutS = 20;
  req.body = "{}";
  const auto res = h.send(req);
  if (!res.reached()) return;
  if (res.status == 401) {
    unauthorized_ = true;
    return;
  }
  const auto doc = json::parseStrict(res.body);
  if ((res.status != 200 && res.status != 201) || !doc || !(*doc)["token"].isString()) {
    log::warn("token: rotation failed (HTTP {})", res.status);
    return;
  }
  std::string err;
  // Written to disk before it is used: a crash in between keeps a token that works (the old one, for 5 min).
  if (!token_.replace((*doc)["token"].asString(), &err)) {
    log::error("token: rotation could not be saved: {}", err);
    setError("token rotation failed: " + err);
    return;
  }
  queue_->setMeta(kMetaTokenIssuedAt, rfc3339(now()));
  log::info("token: rotated");
}

void Daemon::writeStateFile() {
  Json::Value s;
  s["version"] = THELIFE_COLLECTOR_VERSION;
  s["pid"] = static_cast<int>(::getpid());
  s["updated_at"] = rfc3339(now());
  s["queue_depth"] = static_cast<Json::Int64>(queue_ ? queue_->depth() : 0);
  s["online"] = online_.load();
  s["unauthorized"] = unauthorized_.load();
  s["x_ok"] = xOk_.load();
  std::lock_guard lock(mu_);
  s["capture_enabled"] = configKnown_ && config_.enabled;
  s["capture_interval_s"] = config_.captureIntervalS;
  s["last_upload_at"] = lastUploadAt_ ? Json::Value(rfc3339(*lastUploadAt_)) : Json::Value();
  s["last_capture_at"] = lastCaptureAt_ ? Json::Value(rfc3339(*lastCaptureAt_)) : Json::Value();
  s["last_error"] = lastError_.empty() ? Json::Value() : Json::Value(lastError_);
  writeState(s);
}

void Daemon::netLoop() {
  const auto& t = opt_.timing;
  auto h = http(false);  // not aborted on stop: the final flush below still runs
  Uploader up(*queue_, *h, installId_);
  Backoff upBackoff(randomSeed(), Seconds(2), t.backoffCap), cfgBackoff(randomSeed(), Seconds(2), t.backoffCap);
  const auto start = now();
  SysTime nextUpload = start, nextConfig = start, nextHeartbeat = start + Seconds(1), nextRotate = start + Seconds(5),
          nextState = start, nextTokenReload = start;
  bool reportedUnauthorized = false;

  while (!gStop) {
    const auto n = now();
    if (unauthorized_) {
      if (!reportedUnauthorized) {
        log::error("backend: the device token was refused (revoked or expired); uploads stopped. Re-enroll with "
                   "`thelife-collector enroll`.");
        setError("token revoked or expired");
        reportedUnauthorized = true;
        online_ = false;
      }
      if (n >= nextTokenReload) {
        // A re-enrolment writes a new token file; pick it up without a restart.
        const auto before = token_.get();
        std::string err;
        if (token_.load(&err) && token_.get() != before) {
          log::info("token: new token file found; resuming");
          unauthorized_ = false;
          reportedUnauthorized = false;
          configStale_ = true;
          clearError();
        }
        nextTokenReload = n + t.tokenReloadEvery;
      }
    } else {
      if (configStale_.exchange(false) || n >= nextConfig) nextConfig = n + fetchConfig(*h, cfgBackoff);
      if (n >= nextUpload && !unauthorized_) {
        const auto r = up.uploadOnce(n);
        using K = UploadResult::Kind;
        switch (r.kind) {
          case K::Empty: nextUpload = n + t.uploadDelay; break;
          case K::Uploaded:
          case K::Rejected:
            online_ = true;
            upBackoff.reset();
            {
              std::lock_guard lock(mu_);
              lastUploadAt_ = n;
              if (lastError_.starts_with("backend unreachable") || lastError_.starts_with("upload")) lastError_.clear();
            }
            nextUpload = r.sent >= Uploader::kBatch ? n : n + t.uploadDelay;
            break;
          case K::Retry: {
            if (r.error.starts_with("backend unreachable")) online_ = false;
            setError(r.error);
            auto wait = upBackoff.next();
            if (r.retryAfterS) wait = std::max(wait, Seconds(*r.retryAfterS));
            nextUpload = n + wait;
            break;
          }
          case K::Unauthorized: unauthorized_ = true; break;
        }
      }
      if (n >= nextHeartbeat) {
        heartbeat(*h);
        nextHeartbeat = n + t.heartbeatEvery;
      }
      if (n >= nextRotate) {
        maybeRotate(*h);
        nextRotate = n + t.rotateCheckEvery;
      }
    }
    if (n >= nextState) {
      writeStateFile();
      nextState = n + t.stateEvery;
    }
    std::unique_lock lock(netMu_);
    netCv_.wait_for(lock, std::chrono::seconds(1), [this] { return gStop.load() || configStale_.load(); });
  }
  // SIGTERM: one last attempt to hand over what is queued (the session end among it).
  if (!unauthorized_) up.uploadOnce(now());
}

void Daemon::ack(IHttp& h, const Note& n, bool clicked) {
  Json::Value body;
  body["shown_at"] = rfc3339(now());
  body["clicked"] = clicked;
  HttpRequest req;
  req.method = "POST";
  req.path = "/devices/self/notifications/" + n.id + ":ack";  // id validated as a UUID
  req.timeoutS = 15;
  req.body = json::compact(body);
  h.send(req);
}

void Daemon::pollerLoop() {
  auto h = http(true);
  Backoff b(randomSeed(), Seconds(2), opt_.timing.backoffCap);
  while (!gStop) {
    if (unauthorized_) {
      sleepUnlessStopped(Seconds(30));
      continue;
    }
    HttpRequest req;
    req.path = "/devices/self/notifications?wait=55";
    req.timeoutS = 70;
    const auto res = h->send(req);
    if (gStop) break;
    if (!res.reached()) {
      sleepUnlessStopped(b.next());
      continue;
    }
    if (res.status == 401) {
      unauthorized_ = true;
      continue;
    }
    if (res.status != 200) {
      sleepUnlessStopped(std::max(b.next(), Seconds(30)));
      continue;
    }
    b.reset();
    const auto doc = json::parseStrict(res.body);
    int skipped = 0;
    const auto notes = doc ? parseNotes(*doc, &skipped) : std::vector<Note>{};
    if (skipped) log::warn("notifications: ignored {} malformed item(s)", skipped);
    bool fresh = false;
    {
      std::lock_guard lock(noteMu_);
      for (const auto& n : notes) fresh = holder_.add(n, now()) || fresh;
    }
    // Held or already shown items come back on every poll until acknowledged; don't spin on them.
    if (!notes.empty() && !fresh) sleepUnlessStopped(Seconds(30));
  }
}

void Daemon::busLoop(std::unique_ptr<DesktopNotifier> dn) {
  auto h = http(true);
  std::map<uint32_t, Note> shown;
  std::vector<pid_t> children;
  std::vector<uint32_t> clicked;
  while (!gStop) {
    dn->process(1000, [&](uint32_t id) { clicked.push_back(id); });
    for (const auto id : clicked) {
      const auto it = shown.find(id);
      if (it == shown.end()) continue;
      if (const auto url = clickUrl(cfg_.webUiUrl, it->second)) {
        // A fixed program and a URL built here from local configuration: nothing from the backend is executed.
        pid_t pid = 0;
        std::string u = *url;
        char prog[] = "xdg-open";
        char* argv[] = {prog, u.data(), nullptr};
        if (::posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0) children.push_back(pid);
      }
      ack(*h, it->second, true);
      shown.erase(it);
    }
    clicked.clear();
    std::vector<Note> due;
    {
      std::lock_guard lock(noteMu_);
      due = holder_.due(now(), userPresent_);
    }
    for (const auto& n : due) {
      if (const auto id = dn->show(n)) {
        shown[*id] = n;
        if (shown.size() > 100) shown.erase(shown.begin());
        ack(*h, n, false);
      }
    }
    std::erase_if(children, [](pid_t p) { return ::waitpid(p, nullptr, WNOHANG) != 0; });
  }
}

}  // namespace

int runDaemon(const LocalConfig& cfg, TokenStore& token, const DaemonOptions& opt) {
  // No core dumps or ptrace attach by other processes of this user: memory holds screen pixels and the token.
  ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  ::umask(077);
  struct sigaction sa{};
  sa.sa_handler = onSignal;
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);
  ::signal(SIGPIPE, SIG_IGN);
  gStop = false;
  Daemon d(cfg, token, opt);
  return d.run();
}

}  // namespace thelife::collector
