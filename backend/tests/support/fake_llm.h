#pragma once

#include <sys/types.h>

#include <json/value.h>

#include <string>

namespace thelife::test {

// docker/fake-llm/fake_llm.py on a free loopback port (agent_lld.md §3.7). Scripted, FIFO responses.
class FakeLlm {
 public:
  FakeLlm();
  ~FakeLlm();
  FakeLlm(const FakeLlm&) = delete;
  FakeLlm& operator=(const FakeLlm&) = delete;

  std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }
  // Replaces the script (see fake_llm.py for the entry format).
  void script(const Json::Value& responses);
  void reset();
  // Request bodies the server received, oldest first.
  Json::Value requests() const;

 private:
  std::string root() const { return "http://127.0.0.1:" + std::to_string(port_); }
  pid_t pid_ = -1;
  int port_ = 0;
};

}  // namespace thelife::test
