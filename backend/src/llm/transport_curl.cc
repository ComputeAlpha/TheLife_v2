#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>

#include "llm/transport.h"

namespace thelife::llm {

namespace {

void globalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Exchange {
  HttpResult result;
  const std::function<void(std::string_view)>* onChunk = nullptr;
  CURL* curl = nullptr;
  bool statusKnown = false;

  bool success() {
    if (!statusKnown) {
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      result.status = static_cast<int>(code);
      statusKnown = code != 0;
    }
    return result.status >= 200 && result.status < 300;
  }
};

size_t onBody(char* data, size_t size, size_t n, void* user) {
  auto* ex = static_cast<Exchange*>(user);
  const size_t len = size * n;
  // Error bodies are kept whole (bounded) for classification; successful streams go to the consumer.
  if (ex->onChunk && *ex->onChunk && ex->success()) {
    (*ex->onChunk)(std::string_view(data, len));
  } else if (ex->result.body.size() < 4 * 1024 * 1024) {
    ex->result.body.append(data, len);
  }
  return len;
}

size_t onHeader(char* data, size_t size, size_t n, void* user) {
  auto* ex = static_cast<Exchange*>(user);
  const std::string line(data, size * n);
  if (line.rfind("HTTP/", 0) == 0) {
    ex->result.headers.clear();  // a new response (after 100-continue or a redirect)
    ex->statusKnown = false;
    return size * n;
  }
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string value = line.substr(colon + 1);
    const auto b = value.find_first_not_of(" \t");
    const auto e = value.find_last_not_of(" \t\r\n");
    ex->result.headers[name] = b == std::string::npos ? "" : value.substr(b, e - b + 1);
  }
  return size * n;
}

class CurlTransport final : public Transport {
 public:
  explicit CurlTransport(std::string unixSocket) : unixSocket_(std::move(unixSocket)) {}
  HttpResult post(const std::string& url, const std::map<std::string, std::string>& headers,
                  const std::string& body, int timeoutSec, int idleTimeoutSec,
                  const std::function<void(std::string_view)>& onChunk) override {
    globalInit();
    Exchange ex;
    ex.onChunk = &onChunk;
    CURL* curl = curl_easy_init();
    if (!curl) {
      ex.result.transportError = "curl_easy_init failed";
      return ex.result;
    }
    ex.curl = curl;
    curl_slist* list = nullptr;
    for (const auto& [k, v] : headers) list = curl_slist_append(list, (k + ": " + v).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ex);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ex);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    // Idle guard: abort when fewer than 1 byte/s arrives for idleTimeoutSec.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(idleTimeoutSec));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    if (!unixSocket_.empty()) curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, unixSocket_.c_str());
    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
      ex.result.status = 0;
      ex.result.transportError = curl_easy_strerror(rc);
    } else {
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      ex.result.status = static_cast<int>(code);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return ex.result;
  }

 private:
  std::string unixSocket_;
};

}  // namespace

std::unique_ptr<Transport> makeCurlTransport(std::string unixSocket) {
  return std::make_unique<CurlTransport>(std::move(unixSocket));
}

}  // namespace thelife::llm
