#include "http/route.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <algorithm>

#include "auth/authenticator.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"

namespace thelife::http {

Json::Value RequestContext::jsonBody(std::initializer_list<const char*> contentTypes) const {
  const std::string ct(req->getHeader("content-type"));
  const auto base = ct.substr(0, ct.find(';'));
  const bool ok = std::any_of(contentTypes.begin(), contentTypes.end(), [&](const char* t) { return base == t; });
  if (!ok) {
    throw ApiError{415, "unsupported-media-type", "Unsupported media type",
                   "Expected " + std::string(*contentTypes.begin()) + "."};
  }
  Json::Value v;
  try {
    v = json::parse(req->body());
  } catch (const std::invalid_argument&) {
    throw ApiError::badRequest("Request body is not valid JSON.");
  }
  if (!v.isObject()) throw ApiError::badRequest("Request body must be a JSON object.");
  return v;
}

std::string RequestContext::clientIp() const { return req->getPeerAddr().toIp(); }

void addRoute(const std::string& path, drogon::HttpMethod method, Access access, RouteHandler handler,
              std::vector<auth::PrincipalKind> allowedKinds) {
  drogon::app().registerHandler(
      path,
      [handler = std::move(handler), access, allowedKinds = std::move(allowedKinds)](
          drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
        RequestContext ctx{req, ids::newUuidV7(), std::nullopt};
        drogon::HttpResponsePtr resp;
        std::optional<ApiError> apiError;
        bool internal = false;
        try {
          if (access == Access::Authenticated) {
            ctx.principal = co_await auth::authenticate(req);
            if (!ctx.principal) throw ApiError::unauthorized();
            if (std::find(allowedKinds.begin(), allowedKinds.end(), ctx.principal->kind) == allowedKinds.end()) {
              throw ApiError::forbidden("This endpoint is not available to this kind of token.");
            }
          }
          resp = co_await handler(ctx);
        } catch (const ApiError& e) {
          apiError = e;
        } catch (const std::exception& e) {
          LOG_ERROR << "request_id=" << ctx.requestId << " unhandled: " << e.what();
          internal = true;
        }
        if (apiError) resp = problemResponse(*apiError, ctx.requestId);
        if (internal) resp = internalErrorResponse(ctx.requestId);
        resp->addHeader("X-Request-Id", ctx.requestId);
        resp->addHeader("Cache-Control", "no-store");
        co_return resp;
      },
      {method});
}

drogon::HttpResponsePtr jsonResponse(const Json::Value& body, drogon::HttpStatusCode status) {
  auto r = drogon::HttpResponse::newHttpJsonResponse(body);
  r->setStatusCode(status);
  return r;
}

drogon::HttpResponsePtr noContent() {
  auto r = drogon::HttpResponse::newHttpResponse();
  r->setStatusCode(drogon::k204NoContent);
  return r;
}

void BodyReader::rejectUnknown() const {
  std::vector<FieldError> errs;
  for (const auto& name : body.getMemberNames()) {
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      errs.push_back({"/" + name, "unknown property"});
    }
  }
  if (!errs.empty()) throw ApiError::validation(std::move(errs));
}

std::optional<std::string> BodyReader::string(const char* key, size_t minLen, size_t maxLen, bool required) const {
  if (!body.isMember(key)) {
    if (required) throw ApiError::validation({{std::string("/") + key, "is required"}});
    return std::nullopt;
  }
  const auto& v = body[key];
  if (!v.isString()) throw ApiError::validation({{std::string("/") + key, "must be a string"}});
  auto s = v.asString();
  if (s.size() < minLen || s.size() > maxLen) {
    throw ApiError::validation({{std::string("/") + key, "length must be between " + std::to_string(minLen) +
                                                             " and " + std::to_string(maxLen)}});
  }
  return s;
}

std::optional<int> BodyReader::integer(const char* key, int min, int max, bool required) const {
  if (!body.isMember(key)) {
    if (required) throw ApiError::validation({{std::string("/") + key, "is required"}});
    return std::nullopt;
  }
  const auto& v = body[key];
  if (!v.isInt()) throw ApiError::validation({{std::string("/") + key, "must be an integer"}});
  const int i = v.asInt();
  if (i < min || i > max) {
    throw ApiError::validation(
        {{std::string("/") + key, "must be between " + std::to_string(min) + " and " + std::to_string(max)}});
  }
  return i;
}

}  // namespace thelife::http
