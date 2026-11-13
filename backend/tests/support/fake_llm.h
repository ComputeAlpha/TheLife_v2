#pragma once

#include <sys/types.h>

#include <json/value.h>

#include <cstdint>
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

// docker/fake-telegram/fake_telegram.py on a free loopback port: the Bot API the backend long-polls.
class FakeTelegram {
 public:
  static constexpr const char* kToken = "123456:TEST-token";
  FakeTelegram();
  ~FakeTelegram();
  FakeTelegram(const FakeTelegram&) = delete;
  FakeTelegram& operator=(const FakeTelegram&) = delete;

  std::string apiBase() const { return root(); }
  // Queues an update; returns its update_id.
  int64_t push(const Json::Value& update);
  // A private-chat text message from `chatId` (also the sender's user id).
  int64_t message(int64_t chatId, const std::string& text, const std::string& username = "tester");
  int64_t callback(int64_t chatId, const std::string& data, int64_t messageId = 1);
  Json::Value calls() const;                            // [{method, body}]
  Json::Value calls(const std::string& method) const;   // bodies of one method
  void failNext(const std::string& method, int count = 1);

 private:
  std::string root() const { return "http://127.0.0.1:" + std::to_string(port_); }
  pid_t pid_ = -1;
  int port_ = 0;
  int64_t nextMessageId_ = 1;
};

}  // namespace thelife::test
