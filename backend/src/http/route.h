#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "auth/principal.h"

namespace thelife::http {

struct RequestContext {
  drogon::HttpRequestPtr req;
  std::string requestId;
  std::optional<auth::Principal> principal;

  const auth::Principal& who() const { return *principal; }
  // Parses the body as a JSON object; 400 when the content type is wrong or the JSON is malformed.
  Json::Value jsonBody(std::initializer_list<const char*> contentTypes = {"application/json"}) const;
  std::string clientIp() const;
};

using RouteHandler = std::function<drogon::Task<drogon::HttpResponsePtr>(RequestContext&)>;

enum class Access { Public, Authenticated };

// Registers a handler wrapped with: request id (X-Request-Id), optional bearer authentication, a principal-kind
// check, and mapping of ApiError / unexpected exceptions to application/problem+json. Unexpected exceptions are
// logged with the request id and never leak details to the client.
void addRoute(const std::string& path, drogon::HttpMethod method, Access access, RouteHandler handler,
              std::vector<auth::PrincipalKind> allowedKinds = {auth::PrincipalKind::User});

drogon::HttpResponsePtr jsonResponse(const Json::Value& body, drogon::HttpStatusCode status = drogon::k200OK);
drogon::HttpResponsePtr noContent();

// Strict accessors for hand-validated bodies (endpoints outside the tool registry).
struct BodyReader {
  const Json::Value& body;
  std::vector<std::string> allowed;
  void rejectUnknown() const;  // 422 for properties not in `allowed`
  std::optional<std::string> string(const char* key, size_t minLen, size_t maxLen, bool required) const;
  std::optional<int> integer(const char* key, int min, int max, bool required) const;
};

}  // namespace thelife::http
