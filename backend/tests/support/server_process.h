#pragma once

#include <sys/types.h>

#include <map>
#include <string>

namespace thelife::test {

// KEK of the shared test database (base64 of "thelife-test-kek-not-a-secret!!!"); test servers get it by default.
inline constexpr const char* kTestKek = "dGhlbGlmZS10ZXN0LWtlay1ub3QtYS1zZWNyZXQhISE=";

// Runs the real thelife-backend binary on a free loopback port for black-box API tests.
class ServerProcess {
 public:
  explicit ServerProcess(std::map<std::string, std::string> env = {});
  ~ServerProcess();
  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  int port() const { return port_; }
  std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  void waitUntilServing();
  pid_t pid_ = -1;
  int port_ = 0;
};

int pickFreePort();

}  // namespace thelife::test
