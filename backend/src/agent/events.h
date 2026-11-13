#pragma once

#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace thelife::agent {

// Per-user server-sent events (agent_lld.md §7.1): every open stream of a user gets every event of that user.
// Thread-safe; publish() is called from the LLM pool and event loops.
class EventHub {
 public:
  static EventHub& instance();

  void subscribe(const std::string& userId, drogon::ResponseStreamPtr stream);
  void publish(const std::string& userId, const std::string& event, const Json::Value& data);
  // SSE comment to every stream, so proxies and clients see a live connection; drops closed streams.
  void heartbeat();
  std::size_t subscribers(const std::string& userId);

 private:
  bool sendLocked(std::vector<std::shared_ptr<drogon::ResponseStream>>& streams, const std::string& payload);
  std::mutex mu_;
  std::map<std::string, std::vector<std::shared_ptr<drogon::ResponseStream>>> streams_;
};

}  // namespace thelife::agent
