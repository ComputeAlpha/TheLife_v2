#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "net/http.h"
#include "x11/xsource.h"

namespace thelife::collector::testing {

// A scripted IHttp: records every request and answers from `respond` (default 200 "{}").
class FakeHttp : public IHttp {
 public:
  std::vector<HttpRequest> requests;
  std::function<HttpResponse(const HttpRequest&)> respond;
  HttpResponse send(const HttpRequest& req) override {
    HttpRequest copy = req;
    for (auto& p : copy.parts) p.data = nullptr;  // borrowed; gone after the call
    requests.push_back(copy);
    if (respond) return respond(req);
    HttpResponse r;
    r.status = 200;
    r.body = "{}";
    return r;
  }
};

inline HttpResponse response(int status, std::string body = "{}", std::map<std::string, std::string> headers = {}) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  r.headers = std::move(headers);
  return r;
}

// A scriptable X server for the probes.
class FakeX : public IXSource {
 public:
  uint32_t active = 0;
  std::map<uint32_t, WindowInfo> windows;
  IdleInfo idleInfo;
  std::vector<Monitor> mons{{0, {0, 0, 1920, 1080}, true}};
  std::vector<int> pids;

  uint32_t activeWindow() override { return active; }
  WindowInfo window(uint32_t id) override {
    auto it = windows.find(id);
    if (it == windows.end()) return WindowInfo{id, "", "", "", std::nullopt, false, std::nullopt};
    return it->second;
  }
  IdleInfo idle() override { return idleInfo; }
  std::vector<Monitor> monitors() override { return mons; }
  std::vector<int> clientPids() override { return pids; }
  std::optional<Image> grab(const Rect&, std::string* err) override {
    if (err) *err = "fake";
    return std::nullopt;
  }
};

// A temporary directory removed at the end of the test.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "thelife-collector-test-XXXXXX").string();
    path = ::mkdtemp(tmpl.data());
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string str() const { return path.string(); }
};

}  // namespace thelife::collector::testing
