#pragma once

#include <json/value.h>

#include <string>
#include <utility>
#include <vector>

namespace thelife::test {

struct HttpResult {
  int status = 0;  // 0 = connection failed
  std::string contentType;
  std::string body;
  Json::Value json;  // null unless the body parsed as JSON
  std::string header(const std::string& name) const;
  std::vector<std::pair<std::string, std::string>> headers;
};

// Blocking requests for tests. They use their own event loop thread, so they are safe from the test thread.
HttpResult httpGet(const std::string& url, double timeoutSec = 2.0);

struct Request {
  std::string method = "GET";
  std::string url;
  std::string bearer;        // Authorization: Bearer <bearer> when non-empty
  std::string body;          // sent as-is
  std::string contentType = "application/json";
  std::vector<std::pair<std::string, std::string>> headers;
  double timeoutSec = 5.0;
};
HttpResult send(const Request& r);
// Convenience: JSON body from a Json::Value.
HttpResult sendJson(const std::string& method, const std::string& url, const Json::Value& body,
                    const std::string& bearer = {}, const std::string& contentType = "application/json");

}  // namespace thelife::test
