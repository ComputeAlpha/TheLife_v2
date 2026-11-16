#pragma once

#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace thelife {

struct FieldError {
  std::string path;  // JSON Pointer into the body, or a parameter/header name
  std::string message;
};

// An error that maps to an RFC 9457 problem response. Throw it from any handler or service; the route wrapper
// turns it into application/problem+json. `detail` must be safe to show to the client.
class ApiError : public std::runtime_error {
 public:
  ApiError(int status, std::string slug, std::string title, std::string detail = {},
           std::vector<FieldError> errors = {})
      : std::runtime_error(title),
        status_(status),
        slug_(std::move(slug)),
        title_(std::move(title)),
        detail_(std::move(detail)),
        errors_(std::move(errors)) {}

  int status() const { return status_; }
  const std::string& slug() const { return slug_; }
  const std::string& title() const { return title_; }
  const std::string& detail() const { return detail_; }
  const std::vector<FieldError>& errors() const { return errors_; }

  static ApiError badRequest(std::string detail);
  static ApiError unauthorized(std::string detail = "Missing, invalid or expired token.");
  static ApiError forbidden(std::string detail = "Not allowed for this token.");
  static ApiError notFound(std::string detail = "Not found.");
  static ApiError conflict(std::string detail);
  static ApiError validation(std::vector<FieldError> errors);
  static ApiError preconditionFailed(std::string detail = "If-Match does not match the current version.");
  static ApiError preconditionRequired();
  static ApiError tooManyRequests(int retryAfterSec);
  static ApiError serviceUnavailable(std::string detail, int retryAfterSec);

  int retryAfter() const { return retryAfter_; }

 private:
  int status_;
  std::string slug_, title_, detail_;
  std::vector<FieldError> errors_;
  int retryAfter_ = 0;
};

drogon::HttpResponsePtr problemResponse(const ApiError& e, const std::string& requestId);
drogon::HttpResponsePtr internalErrorResponse(const std::string& requestId);

}  // namespace thelife
