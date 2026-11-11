#pragma once

#include <json/value.h>

#include <string>

namespace thelife::test {

struct HttpResult {
  int status = 0;  // 0 = connection failed
  std::string contentType;
  std::string body;
  Json::Value json;  // null unless the body parsed as JSON
};

// Blocking GET for tests. Uses its own event loop thread, so it is safe from the test thread.
HttpResult httpGet(const std::string& url, double timeoutSec = 2.0);

}  // namespace thelife::test
