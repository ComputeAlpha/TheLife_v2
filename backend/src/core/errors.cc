#include "core/errors.h"

namespace thelife {

namespace {
constexpr const char* kTypeBase = "https://thelife.dev/problems/";
}

ApiError ApiError::badRequest(std::string detail) {
  return {400, "bad-request", "Bad request", std::move(detail)};
}
ApiError ApiError::unauthorized(std::string detail) {
  return {401, "unauthorized", "Unauthorized", std::move(detail)};
}
ApiError ApiError::forbidden(std::string detail) { return {403, "forbidden", "Forbidden", std::move(detail)}; }
ApiError ApiError::notFound(std::string detail) { return {404, "not-found", "Not found", std::move(detail)}; }
ApiError ApiError::conflict(std::string detail) { return {409, "conflict", "Conflict", std::move(detail)}; }
ApiError ApiError::validation(std::vector<FieldError> errors) {
  return {422, "validation-failed", "Validation failed", "The request did not pass validation.", std::move(errors)};
}
ApiError ApiError::preconditionFailed(std::string detail) {
  return {412, "precondition-failed", "Precondition failed", std::move(detail)};
}
ApiError ApiError::preconditionRequired() {
  return {428, "precondition-required", "Precondition required", "This request requires an If-Match header."};
}
ApiError ApiError::tooManyRequests(int retryAfterSec) {
  ApiError e{429, "too-many-requests", "Too many requests", "Try again later."};
  e.retryAfter_ = retryAfterSec;
  return e;
}

ApiError ApiError::serviceUnavailable(std::string detail, int retryAfterSec) {
  ApiError e{503, "service-unavailable", "Service unavailable", std::move(detail)};
  e.retryAfter_ = retryAfterSec;
  return e;
}

drogon::HttpResponsePtr problemResponse(const ApiError& e, const std::string& requestId) {
  Json::Value body;
  body["type"] = std::string(kTypeBase) + e.slug();
  body["title"] = e.title();
  body["status"] = e.status();
  if (!e.detail().empty()) body["detail"] = e.detail();
  body["request_id"] = requestId;
  if (!e.errors().empty()) {
    Json::Value errs(Json::arrayValue);
    for (const auto& fe : e.errors()) {
      Json::Value item;
      item["path"] = fe.path;
      item["message"] = fe.message;
      errs.append(item);
    }
    body["errors"] = errs;
  }
  auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(e.status()));
  resp->setContentTypeString("application/problem+json");
  if (e.retryAfter() > 0) resp->addHeader("Retry-After", std::to_string(e.retryAfter()));
  return resp;
}

drogon::HttpResponsePtr internalErrorResponse(const std::string& requestId) {
  return problemResponse(ApiError{500, "internal", "Internal server error",
                                  "Something went wrong. Quote the request id when reporting it."},
                         requestId);
}

}  // namespace thelife
