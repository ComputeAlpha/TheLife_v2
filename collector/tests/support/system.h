#pragma once

#include <json/json.h>
#include <sys/types.h>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "support/fakes.h"

struct xcb_connection_t;

namespace thelife::collector::testing {

// A child process with extra environment; stderr and stdout go to `logPath` when given.
class Child {
 public:
  Child() = default;
  Child(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env,
        const std::string& logPath = "", const std::string& stdinText = "");
  ~Child();
  Child(Child&& o) noexcept : pid_(o.pid_), status_(o.status_) { o.pid_ = -1; }
  Child& operator=(Child&& o) noexcept;
  pid_t pid() const { return pid_; }
  bool running();
  // Sends `sig` and waits; returns the exit status (-1 on timeout, then SIGKILL).
  int stop(int sig = 15, std::chrono::milliseconds timeout = std::chrono::seconds(15));
  std::optional<int> waitExit(std::chrono::milliseconds timeout);

 private:
  pid_t pid_ = -1;
  int status_ = -1;
};

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout,
               std::chrono::milliseconds step = std::chrono::milliseconds(100));
std::string readFile(const std::string& path);

// Xvfb on a free display (-displayfd), 3840x1080 split into two RandR monitors.
class Xvfb {
 public:
  Xvfb();
  ~Xvfb();
  bool ok() const { return !display_.empty(); }
  const std::string& display() const { return display_; }

 private:
  Child proc_;
  std::string display_;
};

// A client that makes windows the way an application and a window manager would (no real WM is running).
class XClient {
 public:
  explicit XClient(const std::string& display);
  ~XClient();
  uint32_t createWindow(int x, int y, int w, int h, uint32_t rgb, const std::string& wmClass,
                        const std::string& title, std::optional<uint32_t> pid);
  void setTitle(uint32_t win, const std::string& title);
  void setFullscreen(uint32_t win, bool on);
  void activate(uint32_t win);  // _NET_ACTIVE_WINDOW and _NET_CLIENT_LIST on the root
  void fakeMotion(int x, int y);
  void sync();

 private:
  uint32_t atom(const char* name);
  xcb_connection_t* c_ = nullptr;
  uint32_t root_ = 0;
  std::vector<uint32_t> clients_;
};

// fake_backend.py on a port; restartable on the same port.
class FakeBackend {
 public:
  explicit FakeBackend(int port = 0);
  ~FakeBackend();
  bool start();
  void stop();
  int port() const { return port_; }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  Json::Value requests();
  std::vector<Json::Value> requests(const std::string& method, const std::string& path);
  // Every observation in every upload, in order.
  std::vector<Json::Value> observations();
  Json::Value state();
  void set(const Json::Value& settings);
  void clear();

 private:
  pid_t pid_ = -1;
  int port_;
};

// curl helpers for the control endpoints.
std::pair<int, std::string> httpRequest(const std::string& method, const std::string& url, const std::string& body = "");

constexpr const char* kToken = "tld_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

}  // namespace thelife::collector::testing
