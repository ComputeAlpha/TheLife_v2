#include "agent/events.h"

#include "core/json.h"

namespace thelife::agent {

EventHub& EventHub::instance() {
  static EventHub hub;
  return hub;
}

void EventHub::subscribe(const std::string& userId, drogon::ResponseStreamPtr stream) {
  std::shared_ptr<drogon::ResponseStream> s(std::move(stream));
  // Reconnect hint and an initial comment so the client knows the stream is open.
  if (!s->send("retry: 3000\n: connected\n\n")) return;
  std::lock_guard lock(mu_);
  streams_[userId].push_back(std::move(s));
}

bool EventHub::sendLocked(std::vector<std::shared_ptr<drogon::ResponseStream>>& streams, const std::string& payload) {
  std::erase_if(streams, [&](const std::shared_ptr<drogon::ResponseStream>& s) { return !s->send(payload); });
  return !streams.empty();
}

void EventHub::publish(const std::string& userId, const std::string& event, const Json::Value& data) {
  const std::string payload = "event: " + event + "\ndata: " + json::compact(data) + "\n\n";
  std::lock_guard lock(mu_);
  auto it = streams_.find(userId);
  if (it == streams_.end()) return;
  if (!sendLocked(it->second, payload)) streams_.erase(it);
}

void EventHub::heartbeat() {
  std::lock_guard lock(mu_);
  for (auto it = streams_.begin(); it != streams_.end();) {
    if (!sendLocked(it->second, ": ping\n\n")) it = streams_.erase(it);
    else ++it;
  }
}

std::size_t EventHub::subscribers(const std::string& userId) {
  std::lock_guard lock(mu_);
  const auto it = streams_.find(userId);
  return it == streams_.end() ? 0 : it->second.size();
}

}  // namespace thelife::agent
