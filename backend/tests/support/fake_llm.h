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
  // Web search endpoints (/api/web_search, /api/web_fetch): scripted bodies, and the calls received.
  void webScript(const Json::Value& responses);
  Json::Value webRequests() const;
  std::string webBaseUrl() const { return root() + "/api"; }
  // Request bodies the server received, oldest first.
  Json::Value requests() const;

 private:
  std::string root() const { return "http://127.0.0.1:" + std::to_string(port_); }
  pid_t pid_ = -1;
  int port_ = 0;
};

// docker/fake-embedder/fake_embedder.py on a socket in a fresh temporary directory.
class FakeEmbedder {
 public:
  FakeEmbedder();
  ~FakeEmbedder();
  FakeEmbedder(const FakeEmbedder&) = delete;
  FakeEmbedder& operator=(const FakeEmbedder&) = delete;
  const std::string& socket() const { return socket_; }
  void stop();

 private:
  pid_t pid_ = -1;
  std::string dir_, socket_;
};

}  // namespace thelife::test
