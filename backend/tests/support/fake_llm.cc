#include "support/fake_llm.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "support/http.h"
#include "support/server_process.h"

extern char** environ;

namespace thelife::test {

FakeLlm::FakeLlm() : port_(pickFreePort()) {
  std::string py = "python3";
  std::string isolated = "-I";  // ignore PYTHONPATH and user site-packages: stdlib only
  std::string script = THELIFE_FAKE_LLM;
  std::string portFlag = "--port";
  std::string port = std::to_string(port_);
  char* argv[] = {py.data(), isolated.data(), script.data(), portFlag.data(), port.data(), nullptr};
  if (::posix_spawnp(&pid_, "python3", nullptr, nullptr, argv, environ) != 0) {
    throw std::runtime_error("failed to spawn fake-llm");
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (httpGet(root() + "/_fake/health").status == 200) return;
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      throw std::runtime_error("fake-llm exited during startup");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("fake-llm did not start within 10s");
}

FakeLlm::~FakeLlm() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  ::waitpid(pid_, nullptr, 0);
}

void FakeLlm::script(const Json::Value& responses) {
  Json::Value body;
  body["responses"] = responses;
  if (sendJson("POST", root() + "/_fake/script", body).status != 200) throw std::runtime_error("fake-llm script");
}

void FakeLlm::reset() { sendJson("POST", root() + "/_fake/reset", Json::Value(Json::objectValue)); }

Json::Value FakeLlm::requests() const { return httpGet(root() + "/_fake/requests").json; }

}  // namespace thelife::test
