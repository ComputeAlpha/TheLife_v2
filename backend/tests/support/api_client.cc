#include "support/api_client.h"

#include <json/writer.h>

#include <stdexcept>

namespace thelife::test {

namespace {
std::string compact(const Json::Value& v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  return Json::writeString(w, v);
}
}  // namespace

ApiClient::ApiClient(const ServerProcess& server, const TestUser& user)
    : base_(server.baseUrl() + "/api/v1"), user_(user) {
  Json::Value b;
  b["email"] = user.email;
  b["password"] = user.password;
  const auto r = sendJson("POST", base_ + "/auth/login", b);
  if (r.status != 200) throw std::runtime_error("login failed: " + r.body);
  token_ = r.json["token"].asString();
}

HttpResult ApiClient::get(const std::string& path) const {
  Request r;
  r.url = base_ + path;
  r.bearer = token_;
  return send(r);
}

HttpResult ApiClient::post(const std::string& path, const Json::Value& body, const std::string& key) const {
  Request r;
  r.method = "POST";
  r.url = base_ + path;
  r.bearer = token_;
  r.body = compact(body);
  if (!key.empty()) r.headers.emplace_back("Idempotency-Key", key);
  return send(r);
}

HttpResult ApiClient::patch(const std::string& path, const Json::Value& body, const std::string& ifMatch) const {
  Request r;
  r.method = "PATCH";
  r.url = base_ + path;
  r.bearer = token_;
  r.body = compact(body);
  r.contentType = "application/merge-patch+json";
  if (!ifMatch.empty()) r.headers.emplace_back("If-Match", ifMatch);
  return send(r);
}

HttpResult ApiClient::del(const std::string& path, const std::string& ifMatch) const {
  Request r;
  r.method = "DELETE";
  r.url = base_ + path;
  r.bearer = token_;
  if (!ifMatch.empty()) r.headers.emplace_back("If-Match", ifMatch);
  return send(r);
}

std::unique_ptr<ServerProcess> startServerWithDb() {
  return std::make_unique<ServerProcess>(std::map<std::string, std::string>{{"THELIFE_DB_URL", appDbUrl()}});
}

}  // namespace thelife::test
