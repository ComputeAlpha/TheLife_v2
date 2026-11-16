#include "support/system.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/json.h"

extern char** environ;

namespace thelife::collector::testing {

Child::Child(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env,
             const std::string& logPath, const std::string& stdinText) {
  std::map<std::string, std::string> merged;
  for (char** e = environ; *e; ++e) {
    const std::string kv = *e;
    const auto eq = kv.find('=');
    if (eq != std::string::npos) merged[kv.substr(0, eq)] = kv.substr(eq + 1);
  }
  for (const auto& [k, v] : env) {
    if (v.empty()) merged.erase(k);
    else merged[k] = v;
  }
  std::vector<std::string> envStr;
  for (const auto& [k, v] : merged) envStr.push_back(k + "=" + v);
  std::vector<char*> envp, args;
  for (auto& s : envStr) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::vector<std::string> argvCopy = argv;
  for (auto& s : argvCopy) args.push_back(s.data());
  args.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  int inPipe[2] = {-1, -1};
  if (!stdinText.empty() && ::pipe(inPipe) == 0) {
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    posix_spawn_file_actions_addclose(&fa, inPipe[1]);
  } else {
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  }
  if (!logPath.empty()) {
    posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
  }
  if (posix_spawnp(&pid_, args[0], &fa, nullptr, args.data(), envp.data()) != 0) pid_ = -1;
  posix_spawn_file_actions_destroy(&fa);
  if (inPipe[0] >= 0) {
    ::close(inPipe[0]);
    for (size_t off = 0; off < stdinText.size();) {
      const auto n = ::write(inPipe[1], stdinText.data() + off, stdinText.size() - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    ::close(inPipe[1]);
  }
}

Child& Child::operator=(Child&& o) noexcept {
  if (this != &o) {
    stop(SIGKILL, std::chrono::seconds(2));
    pid_ = o.pid_;
    status_ = o.status_;
    o.pid_ = -1;
  }
  return *this;
}

Child::~Child() { stop(SIGKILL, std::chrono::seconds(2)); }

bool Child::running() {
  if (pid_ <= 0) return false;
  int st = 0;
  const pid_t r = ::waitpid(pid_, &st, WNOHANG);
  if (r == pid_) {
    status_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    pid_ = -1;
    return false;
  }
  return true;
}

std::optional<int> Child::waitExit(std::chrono::milliseconds timeout) {
  if (waitUntil([&] { return !running(); }, timeout, std::chrono::milliseconds(20))) return status_;
  return std::nullopt;
}

int Child::stop(int sig, std::chrono::milliseconds timeout) {
  if (!running()) return status_;
  ::kill(pid_, sig);
  if (const auto s = waitExit(timeout)) return *s;
  ::kill(pid_, SIGKILL);
  waitExit(std::chrono::seconds(5));
  return -1;
}

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout,
               std::chrono::milliseconds step) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (pred()) return true;
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(step);
  }
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

Xvfb::Xvfb() {
  int fds[2];
  if (::pipe(fds) != 0) return;
  // -displayfd writes the display number once the server accepts connections. The fd must survive exec.
  ::fcntl(fds[1], F_SETFD, 0);
  proc_ = Child({"Xvfb", "-displayfd", std::to_string(fds[1]), "-screen", "0", "3840x1080x24", "-nolisten", "tcp", "-noreset",
                 "+extension", "RANDR"},
                {});
  ::close(fds[1]);
  pollfd p{fds[0], POLLIN, 0};
  std::string num;
  if (::poll(&p, 1, 10000) > 0) {
    char buf[16] = {};
    const auto n = ::read(fds[0], buf, sizeof buf - 1);
    if (n > 0) num.assign(buf, static_cast<size_t>(n));
  }
  ::close(fds[0]);
  while (!num.empty() && (num.back() == '\n')) num.pop_back();
  if (num.empty()) return;
  display_ = ":" + num;
  // Two side-by-side monitors on the one screen (collector_lld.md §11).
  Child a({"xrandr", "-d", display_, "--setmonitor", "left", "1920/508x1080/286+0+0", "none"}, {}, "/dev/null");
  a.waitExit(std::chrono::seconds(10));
  // Giving the screen's only output to the second monitor retires Xvfb's automatic whole-screen monitor.
  Child b({"xrandr", "-d", display_, "--setmonitor", "right", "1920/508x1080/286+1920+0", "screen"}, {}, "/dev/null");
  b.waitExit(std::chrono::seconds(10));
}

Xvfb::~Xvfb() { proc_.stop(); }

XClient::XClient(const std::string& display) {
  c_ = xcb_connect(display.c_str(), nullptr);
  root_ = xcb_setup_roots_iterator(xcb_get_setup(c_)).data->root;
}

XClient::~XClient() { xcb_disconnect(c_); }

uint32_t XClient::atom(const char* name) {
  auto* r = xcb_intern_atom_reply(c_, xcb_intern_atom(c_, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
  const uint32_t a = r ? r->atom : 0;
  std::free(r);
  return a;
}

uint32_t XClient::createWindow(int x, int y, int w, int h, uint32_t rgb, const std::string& wmClass,
                               const std::string& title, std::optional<uint32_t> pid) {
  const auto* screen = xcb_setup_roots_iterator(xcb_get_setup(c_)).data;
  const uint32_t win = xcb_generate_id(c_);
  const uint32_t values[] = {rgb, 1u};  // background pixel, override-redirect (no WM to place it)
  xcb_create_window(c_, XCB_COPY_FROM_PARENT, win, root_, static_cast<int16_t>(x), static_cast<int16_t>(y),
                    static_cast<uint16_t>(w), static_cast<uint16_t>(h), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, values);
  const std::string cls = wmClass + std::string(1, '\0') + wmClass + std::string(1, '\0');
  xcb_change_property(c_, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                      static_cast<uint32_t>(cls.size()), cls.data());
  setTitle(win, title);
  if (pid) xcb_change_property(c_, XCB_PROP_MODE_REPLACE, win, atom("_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1, &*pid);
  xcb_map_window(c_, win);
  clients_.push_back(win);
  xcb_change_property(c_, XCB_PROP_MODE_REPLACE, root_, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW, 32,
                      static_cast<uint32_t>(clients_.size()), clients_.data());
  sync();
  return win;
}

void XClient::setTitle(uint32_t win, const std::string& title) {
  xcb_change_property(c_, XCB_PROP_MODE_REPLACE, win, atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8,
                      static_cast<uint32_t>(title.size()), title.data());
  sync();
}

void XClient::setFullscreen(uint32_t win, bool on) {
  const uint32_t fs = atom("_NET_WM_STATE_FULLSCREEN");
  xcb_change_property(c_, XCB_PROP_MODE_REPLACE, win, atom("_NET_WM_STATE"), XCB_ATOM_ATOM, 32, on ? 1 : 0, &fs);
  sync();
}

void XClient::activate(uint32_t win) {
  xcb_change_property(c_, XCB_PROP_MODE_REPLACE, root_, atom("_NET_ACTIVE_WINDOW"), XCB_ATOM_WINDOW, 32, 1, &win);
  sync();
}

void XClient::fakeMotion(int x, int y) {
  xcb_test_fake_input(c_, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, static_cast<int16_t>(x),
                      static_cast<int16_t>(y), 0);
  sync();
}

void XClient::sync() { std::free(xcb_get_input_focus_reply(c_, xcb_get_input_focus(c_), nullptr)); }

FakeBackend::FakeBackend(int port) : port_(port) {}
FakeBackend::~FakeBackend() { stop(); }

bool FakeBackend::start() {
  int fds[2];
  if (::pipe(fds) != 0) return false;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  std::string py = "python3", script = THELIFE_FAKE_BACKEND, port = std::to_string(port_);
  char* argv[] = {py.data(), script.data(), port.data(), nullptr};
  pid_t pid;
  const int rc = posix_spawnp(&pid, "python3", &fa, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    return false;
  }
  std::string line;
  pollfd p{fds[0], POLLIN, 0};
  while (line.find('\n') == std::string::npos && ::poll(&p, 1, 10000) > 0) {
    char buf[64];
    const auto n = ::read(fds[0], buf, sizeof buf);
    if (n <= 0) break;
    line.append(buf, static_cast<size_t>(n));
  }
  ::close(fds[0]);
  if (!line.starts_with("READY ")) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return false;
  }
  port_ = std::atoi(line.c_str() + 6);
  pid_ = pid;
  return true;
}

void FakeBackend::stop() {
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }
}

namespace {
size_t collect(char* p, size_t s, size_t n, void* ud) {
  static_cast<std::string*>(ud)->append(p, s * n);
  return s * n;
}
}  // namespace

std::pair<int, std::string> httpRequest(const std::string& method, const std::string& url, const std::string& body) {
  CURL* c = curl_easy_init();
  std::string out;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/json");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  if (method == "POST") {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  long code = 0;
  if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(h);
  curl_easy_cleanup(c);
  return {static_cast<int>(code), out};
}

Json::Value FakeBackend::requests() {
  const auto [code, body] = httpRequest("GET", url() + "/__control/requests");
  if (code != 200) return Json::Value(Json::arrayValue);
  return json::parseStrict(body, 256 * 1024 * 1024).value_or(Json::Value(Json::arrayValue));
}

std::vector<Json::Value> FakeBackend::requests(const std::string& method, const std::string& path) {
  std::vector<Json::Value> out;
  for (const auto& r : requests()) {
    if (r["method"].asString() == method && r["path"].asString() == path) out.push_back(r);
  }
  return out;
}

std::vector<Json::Value> FakeBackend::observations() {
  std::vector<Json::Value> out;
  for (const auto& r : requests("POST", "/api/v1/ingest/observations")) {
    for (const auto& o : r["json"]["observations"]) out.push_back(o);
  }
  return out;
}

Json::Value FakeBackend::state() {
  const auto [code, body] = httpRequest("GET", url() + "/__control/state");
  return json::parseStrict(body, 64 * 1024 * 1024).value_or(Json::Value());
}

void FakeBackend::set(const Json::Value& settings) { httpRequest("POST", url() + "/__control/set", json::compact(settings)); }
void FakeBackend::clear() { httpRequest("POST", url() + "/__control/clear", "{}"); }

}  // namespace thelife::collector::testing
