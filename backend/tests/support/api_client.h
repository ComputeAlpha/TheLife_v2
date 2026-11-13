#pragma once

#include <json/value.h>

#include <map>
#include <memory>

#include <string>

#include "support/fixtures.h"
#include "support/http.h"
#include "support/server_process.h"

namespace thelife::test {

// A logged-in user talking to a server: small wrapper to keep API tests readable.
class ApiClient {
 public:
  ApiClient(const ServerProcess& server, const TestUser& user);
  // Uses an existing bearer token (device token, API key) instead of logging in.
  ApiClient(const ServerProcess& server, const TestUser& user, std::string token);

  HttpResult get(const std::string& path) const;
  HttpResult post(const std::string& path, const Json::Value& body, const std::string& idempotencyKey = {}) const;
  HttpResult patch(const std::string& path, const Json::Value& body, const std::string& ifMatch) const;
  HttpResult del(const std::string& path, const std::string& ifMatch) const;
  // Any method; body sent when non-null.
  HttpResult call(const std::string& method, const std::string& path, const Json::Value& body = Json::Value(),
                  const std::vector<std::pair<std::string, std::string>>& headers = {}) const;

  const std::string& token() const { return token_; }
  const TestUser& user() const { return user_; }

 private:
  std::string base_;
  TestUser user_;
  std::string token_;
};

// Starts a server bound to the test database.
std::unique_ptr<ServerProcess> startServerWithDb(std::map<std::string, std::string> extraEnv = {});

}  // namespace thelife::test
