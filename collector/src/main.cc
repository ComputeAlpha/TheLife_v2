// thelife-collector: run | enroll | status | pause <duration> | resume (collector_lld.md §2).
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "app/daemon.h"
#include "app/state.h"
#include "config/local_config.h"
#include "config/server_config.h"
#include "config/token_store.h"
#include "core/files.h"
#include "core/json.h"
#include "core/log.h"
#include "net/http.h"
#include "queue/queue.h"

using namespace thelife::collector;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: thelife-collector run\n"
               "       thelife-collector enroll --url URL [--web-ui URL] [--ca-file PATH] [--pin sha256//...]\n"
               "                               (reads the device token from standard input)\n"
               "       thelife-collector status\n"
               "       thelife-collector pause DURATION   (e.g. 30m, 2h, 1d)\n"
               "       thelife-collector resume\n"
               "       thelife-collector --version\n");
  return 64;
}

std::optional<LocalConfig> loadConfig() {
  std::string err;
  const auto text = files::readPrivate(configPath(), &err);
  if (!text) {
    std::fprintf(stderr, "%s\n(enroll first: thelife-collector enroll --url URL)\n", err.c_str());
    return std::nullopt;
  }
  auto cfg = parseLocalConfig(*text, &err);
  if (!cfg) std::fprintf(stderr, "%s\n", err.c_str());
  return cfg;
}

// The token is read from standard input (never argv, collector_lld.md §8), without echo on a terminal.
std::string readToken() {
  const bool tty = ::isatty(STDIN_FILENO);
  termios old{};
  if (tty) {
    std::fprintf(stderr, "Device token: ");
    ::tcgetattr(STDIN_FILENO, &old);
    termios quiet = old;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
  }
  std::string token;
  std::getline(std::cin, token);
  if (tty) {
    ::tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::fprintf(stderr, "\n");
  }
  while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) token.pop_back();
  return token;
}

int enroll(const std::vector<std::string>& args) {
  LocalConfig cfg;
  for (size_t i = 0; i < args.size(); ++i) {
    auto value = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : ""; };
    if (args[i] == "--url") cfg.backendUrl = value();
    else if (args[i] == "--web-ui") cfg.webUiUrl = value();
    else if (args[i] == "--ca-file") cfg.caFile = value();
    else if (args[i] == "--pin") cfg.pinSha256 = value();
    else if (args[i].starts_with("--token")) {
      std::fprintf(stderr, "the token is read from standard input, never from the command line\n");
      return 64;
    } else return usage();
  }
  while (!cfg.backendUrl.empty() && cfg.backendUrl.back() == '/') cfg.backendUrl.pop_back();
  // Round-trip through the parser so enroll accepts exactly what `run` will.
  std::string err;
  const auto checked = parseLocalConfig(renderLocalConfig(cfg), &err);
  if (!checked) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 64;
  }
  cfg = *checked;
  const std::string token = readToken();
  if (!TokenStore::wellFormed(token)) {
    std::fprintf(stderr, "that is not a device token (expected tld_…, from registering a device)\n");
    return 64;
  }

  CurlHttp http(cfg, [&] { return token; }, std::string("thelife-collector/") + THELIFE_COLLECTOR_VERSION);
  HttpRequest req;
  req.path = "/devices/self/config";
  req.timeoutS = 20;
  const auto res = http.send(req);
  if (!res.reached()) {
    std::fprintf(stderr, "cannot reach %s: %s\n", cfg.backendUrl.c_str(), res.error.c_str());
    return 1;
  }
  if (res.status == 401 || res.status == 403) {
    std::fprintf(stderr, "the backend rejected the token (HTTP %d)\n", res.status);
    return 1;
  }
  const auto doc = json::parseStrict(res.body);
  if (res.status != 200 || !doc || !parseServerConfig(*doc, &err)) {
    std::fprintf(stderr, "unexpected answer from %s (HTTP %d); is this a TheLife backend?\n", cfg.backendUrl.c_str(),
                 res.status);
    return 1;
  }

  if (!files::ensurePrivateDir(files::configDir(), &err) || !files::ensurePrivateDir(files::stateDir(), &err) ||
      !files::writePrivateAtomic(configPath(), renderLocalConfig(cfg), &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  TokenStore store(tokenPath());
  if (!store.replace(token, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  try {
    Queue q(queuePath());
    q.setMeta(kMetaTokenIssuedAt, rfc3339(std::chrono::system_clock::now()));
    q.deleteMeta(kMetaServerConfig);
    q.deleteMeta(kMetaConfigEtag);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  std::printf("Enrolled with %s.\nStart it with: systemctl --user enable --now thelife-collector\n",
              cfg.backendUrl.c_str());
  return 0;
}

int status() {
  const auto s = readState();
  std::optional<std::string> paused;
  int64_t depth = -1;
  try {
    Queue q(queuePath());
    paused = q.meta(kMetaPausedUntil);
    depth = q.depth();
  } catch (const std::exception&) {
  }
  if (!s && depth < 0) {
    std::printf("not running (no state yet; enroll and start the service)\n");
    return 3;
  }
  auto str = [](const Json::Value& v) { return v.isNull() ? std::string("-") : v.asString(); };
  if (s) {
    const auto& j = *s;
    std::printf("version:          %s (pid %d)\n", str(j["version"]).c_str(), j["pid"].asInt());
    std::printf("updated:          %s\n", str(j["updated_at"]).c_str());
    std::printf("backend:          %s\n", j["unauthorized"].asBool() ? "token refused (re-enroll)"
                                          : j["online"].asBool()     ? "reachable"
                                                                     : "unreachable");
    std::printf("capture:          %s",
                j["capture_enabled"].asBool() ? "on" : "off");
    if (j["capture_enabled"].asBool()) std::printf(" (every ~%ds)", j["capture_interval_s"].asInt());
    std::printf("\nlast upload:      %s\n", str(j["last_upload_at"]).c_str());
    std::printf("last capture:     %s\n", str(j["last_capture_at"]).c_str());
    std::printf("last error:       %s\n", str(j["last_error"]).c_str());
  }
  if (depth >= 0) std::printf("queued:           %lld observation(s)\n", static_cast<long long>(depth));
  const auto until = paused ? parseRfc3339(*paused) : std::nullopt;
  if (until && *until > std::chrono::system_clock::now()) std::printf("paused until:     %s\n", paused->c_str());
  return 0;
}

int pause(const std::vector<std::string>& args) {
  if (args.size() != 1) return usage();
  const auto d = parseDuration(args[0]);
  if (!d) {
    std::fprintf(stderr, "duration like 30m, 2h or 1d (at most 30d)\n");
    return 64;
  }
  std::string err;
  if (!files::ensurePrivateDir(files::stateDir(), &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  const auto until = std::chrono::system_clock::now() + *d;
  Queue(queuePath()).setMeta(kMetaPausedUntil, rfc3339(until));
  std::printf("Paused for %s (until %s). Nothing is observed or captured meanwhile.\n", formatDuration(*d).c_str(),
              rfc3339(until).c_str());
  return 0;
}

int resume() {
  Queue(queuePath()).deleteMeta(kMetaPausedUntil);
  std::printf("Resumed.\n");
  return 0;
}

int run() {
  const auto cfg = loadConfig();
  if (!cfg) return kExitConfig;
  log::Level lvl = log::Level::Info;
  log::parseLevel(cfg->logLevel, lvl);
  log::setLevel(lvl);
  std::string err;
  TokenStore token(tokenPath());
  if (!token.load(&err)) {
    log::error("{}", err);
    return kExitConfig;
  }
  if (!files::ensurePrivateDir(files::stateDir(), &err)) {
    log::error("{}", err);
    return kExitConfig;
  }
  DaemonOptions opt;
  opt.timing = Timing::fromEnv();
  if (const char* p = std::getenv("THELIFE_COLLECTOR_PROC_ROOT"); p && *p) opt.procRoot = p;
  if (const char* l = std::getenv("THELIFE_COLLECTOR_NO_LOGIND"); l && std::string(l) == "1") opt.useLogind = false;
  return runDaemon(*cfg, token, opt);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return usage();
  const auto cmd = args.front();
  args.erase(args.begin());
  try {
    if (cmd == "run") return run();
    if (cmd == "enroll") return enroll(args);
    if (cmd == "status") return status();
    if (cmd == "pause") return pause(args);
    if (cmd == "resume") return resume();
    if (cmd == "--version" || cmd == "version") {
      std::printf("thelife-collector %s\n", THELIFE_COLLECTOR_VERSION);
      return 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return usage();
}
