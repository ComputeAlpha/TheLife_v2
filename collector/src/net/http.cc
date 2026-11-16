#include "net/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/json.h"

namespace thelife::collector {

std::optional<int> HttpResponse::retryAfterS() const {
  const auto it = headers.find("retry-after");
  if (it == headers.end() || it->second.empty() || it->second.size() > 6) return std::nullopt;
  if (!std::all_of(it->second.begin(), it->second.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  return std::min(std::stoi(it->second), 3600);
}

namespace {

struct Sink {
  std::string* body;
  bool overflow = false;
};

size_t onBody(char* p, size_t size, size_t n, void* ud) {
  auto* s = static_cast<Sink*>(ud);
  const size_t len = size * n;
  if (s->body->size() + len > json::kMaxResponseBytes) {
    s->overflow = true;
    return 0;  // aborts the transfer
  }
  s->body->append(p, len);
  return len;
}

size_t onHeader(char* p, size_t size, size_t n, void* ud) {
  auto* h = static_cast<std::map<std::string, std::string>*>(ud);
  const std::string line(p, size * n);
  if (line.starts_with("HTTP/")) h->clear();  // a new response (100-continue)
  const auto colon = line.find(':');
  if (colon != std::string::npos && h->size() < 64) {
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string value = line.substr(colon + 1);
    const auto b = value.find_first_not_of(" \t"), e = value.find_last_not_of(" \t\r\n");
    (*h)[name] = b == std::string::npos ? "" : value.substr(b, e - b + 1);
  }
  return size * n;
}

struct PartReader {
  const uint8_t* data;
  size_t size, off = 0;
};

size_t readPart(char* buf, size_t size, size_t n, void* ud) {
  auto* r = static_cast<PartReader*>(ud);
  const size_t len = std::min(size * n, r->size - r->off);
  std::memcpy(buf, r->data + r->off, len);
  r->off += len;
  return len;
}

int seekPart(void* ud, curl_off_t offset, int origin) {
  auto* r = static_cast<PartReader*>(ud);
  if (origin != SEEK_SET || offset < 0 || static_cast<size_t>(offset) > r->size) return CURL_SEEKFUNC_FAIL;
  r->off = static_cast<size_t>(offset);
  return CURL_SEEKFUNC_OK;
}

int onProgress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto* abort = static_cast<const std::function<bool()>*>(ud);
  return (*abort && (*abort)()) ? 1 : 0;
}

struct GlobalInit {
  GlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
};

}  // namespace

CurlHttp::CurlHttp(LocalConfig cfg, std::function<std::string()> token, std::string userAgent)
    : cfg_(std::move(cfg)), token_(std::move(token)), userAgent_(std::move(userAgent)) {
  static GlobalInit init;
  curl_ = curl_easy_init();
}

CurlHttp::~CurlHttp() { curl_easy_cleanup(static_cast<CURL*>(curl_)); }

HttpResponse CurlHttp::send(const HttpRequest& req) {
  std::lock_guard lock(mu_);
  HttpResponse res;
  auto* c = static_cast<CURL*>(curl_);
  if (!c) {
    res.error = "curl unavailable";
    return res;
  }
  curl_easy_reset(c);
  const std::string url = cfg_.backendUrl + "/api/v1" + req.path;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, req.timeoutS);
  curl_easy_setopt(c, CURLOPT_USERAGENT, userAgent_.c_str());
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // advertise what libcurl can decode
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &abort_);
  if (url.starts_with("https://")) {
    curl_easy_setopt(c, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!cfg_.caFile.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, cfg_.caFile.c_str());
    if (!cfg_.pinSha256.empty()) curl_easy_setopt(c, CURLOPT_PINNEDPUBLICKEY, cfg_.pinSha256.c_str());
  }

  curl_slist* headers = nullptr;
  const std::string auth = "Authorization: Bearer " + token_();
  headers = curl_slist_append(headers, auth.c_str());
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Expect:");
  for (const auto& [k, v] : req.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());

  curl_mime* mime = nullptr;
  std::vector<PartReader> readers;
  readers.reserve(req.parts.size());
  if (!req.parts.empty()) {
    mime = curl_mime_init(c);
    for (const auto& p : req.parts) {
      auto* part = curl_mime_addpart(mime);
      curl_mime_name(part, p.name.c_str());
      if (p.data) {
        readers.push_back({p.data, p.size});
        curl_mime_data_cb(part, static_cast<curl_off_t>(p.size), readPart, seekPart, nullptr, &readers.back());
        curl_mime_filename(part, p.filename.c_str());
      } else {
        curl_mime_data(part, p.text.c_str(), p.text.size());
      }
      if (!p.contentType.empty()) curl_mime_type(part, p.contentType.c_str());
    }
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
  } else if (req.method == "POST") {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
  } else if (req.method != "GET") {
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

  Sink sink{&res.body};
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &res.headers);

  const CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    res.status = static_cast<int>(code);
  } else {
    res.error = sink.overflow ? "response too large" : curl_easy_strerror(rc);
    res.body.clear();
  }
  // Clear the options that point into this frame (the auth header, the body).
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, nullptr);
  curl_easy_setopt(c, CURLOPT_MIMEPOST, nullptr);
  curl_slist_free_all(headers);
  curl_mime_free(mime);
  return res;
}

}  // namespace thelife::collector
