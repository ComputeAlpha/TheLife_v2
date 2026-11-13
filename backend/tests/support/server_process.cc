#include "support/server_process.h"

#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "support/http.h"

extern char** environ;

namespace thelife::test {

int pickFreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    throw std::runtime_error("bind() failed");
  }
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  ::close(fd);
  return ntohs(addr.sin_port);
}

ServerProcess::ServerProcess(std::map<std::string, std::string> env) : port_(pickFreePort()) {
  env["THELIFE_LISTEN_ADDR"] = "127.0.0.1";
  env["THELIFE_LISTEN_PORT"] = std::to_string(port_);
  // Tests share one database: a background worker would pick up other tests' jobs. Tests that need it opt in.
  if (!env.count("THELIFE_JOBS")) env["THELIFE_JOBS"] = "off";

  // Inherit the parent environment, minus THELIFE_* (tests must be explicit), plus overrides.
  std::vector<std::string> envStrings;
  for (char** e = environ; *e != nullptr; ++e) {
    if (std::string(*e).rfind("THELIFE_", 0) != 0) envStrings.emplace_back(*e);
  }
  for (const auto& [k, v] : env) envStrings.push_back(k + "=" + v);
  std::vector<char*> envp;
  for (auto& s : envStrings) envp.push_back(s.data());
  envp.push_back(nullptr);

  std::string bin = THELIFE_BACKEND_BIN;
  char* argv[] = {bin.data(), nullptr};
  if (::posix_spawn(&pid_, bin.c_str(), nullptr, nullptr, argv, envp.data()) != 0) {
    throw std::runtime_error("failed to spawn " + bin);
  }
  waitUntilServing();
}

void ServerProcess::waitUntilServing() {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      throw std::runtime_error("backend exited during startup");
    }
    if (httpGet(baseUrl() + "/healthz").status == 200) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("backend did not start serving within 10s");
}

ServerProcess::~ServerProcess() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  for (int i = 0; i < 100; ++i) {
    if (::waitpid(pid_, nullptr, WNOHANG) == pid_) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::kill(pid_, SIGKILL);
  ::waitpid(pid_, nullptr, 0);
}

}  // namespace thelife::test
