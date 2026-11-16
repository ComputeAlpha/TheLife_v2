#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/local_config.h"

namespace thelife::collector {

// One part of a multipart body. File data is borrowed: the caller keeps it alive (and zeroes it) and the transport
// streams it without copying (collector_lld.md §5.4).
struct HttpPart {
  std::string name;
  std::string filename;  // empty for a text field
  std::string contentType;
  std::string text;                 // text fields
  const uint8_t* data = nullptr;    // file parts
  size_t size = 0;
};

struct HttpRequest {
  std::string method = "GET";
  std::string path;  // under /api/v1, e.g. "/devices/self/config"
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;  // JSON
  std::vector<HttpPart> parts;  // multipart/form-data when non-empty
  long timeoutS = 30;
};

struct HttpResponse {
  int status = 0;     // 0: transport failure (see error)
  std::string error;  // transport error, never contains the token
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
  bool reached() const { return status > 0; }
  std::optional<int> retryAfterS() const;
};

class IHttp {
 public:
  virtual ~IHttp() = default;
  virtual HttpResponse send(const HttpRequest& req) = 0;
};

// libcurl transport (collector_lld.md §7.4, §8): fixed base URL, no redirects, http(s) only, TLS 1.3 for https,
// optional CA file and SPKI pin, responses capped at 256 KiB. The token is added per request from `token()`.
class CurlHttp : public IHttp {
 public:
  CurlHttp(LocalConfig cfg, std::function<std::string()> token, std::string userAgent);
  ~CurlHttp() override;
  HttpResponse send(const HttpRequest& req) override;
  // Checked while a transfer runs (about once a second); returning true aborts it (shutdown).
  void setAbort(std::function<bool()> abort) { abort_ = std::move(abort); }

 private:
  std::function<bool()> abort_;
  LocalConfig cfg_;
  std::function<std::string()> token_;
  std::string userAgent_;
  std::mutex mu_;
  void* curl_ = nullptr;
};

}  // namespace thelife::collector
