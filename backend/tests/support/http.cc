#include "support/http.h"

#include <drogon/HttpClient.h>
#include <json/reader.h>
#include <trantor/net/EventLoopThread.h>

#include <map>

#include <json/writer.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace thelife::test {

namespace {
trantor::EventLoop* clientLoop() {
  static auto* thread = [] {
    auto* t = new trantor::EventLoopThread("test-http");
    t->run();
    return t;
  }();
  return thread->getLoop();
}

void splitUrl(const std::string& url, std::string& origin, std::string& path) {
  const auto schemeEnd = url.find("://");
  const auto pathStart = url.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
  origin = pathStart == std::string::npos ? url : url.substr(0, pathStart);
  path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
}
}  // namespace

HttpResult send(const Request& r) {
  std::string origin, path;
  splitUrl(r.url, origin, path);
  auto client = drogon::HttpClient::newHttpClient(origin, clientLoop());
  auto req = drogon::HttpRequest::newHttpRequest();
  static const std::map<std::string, drogon::HttpMethod> kMethods{
      {"GET", drogon::Get}, {"POST", drogon::Post}, {"PUT", drogon::Put}, {"PATCH", drogon::Patch},
      {"DELETE", drogon::Delete}, {"OPTIONS", drogon::Options}, {"HEAD", drogon::Head}};
  req->setMethod(kMethods.at(r.method));
  const auto q = path.find('?');
  req->setPath(q == std::string::npos ? path : path.substr(0, q));
  if (q != std::string::npos) {
    // "a=1&b=2" → query parameters (tests use plain values without percent-encoding).
    std::string query = path.substr(q + 1);
    size_t start = 0;
    while (start <= query.size()) {
      const auto amp = query.find('&', start);
      const auto pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
      if (!pair.empty()) {
        const auto eq = pair.find('=');
        req->setParameter(pair.substr(0, eq), eq == std::string::npos ? "" : pair.substr(eq + 1));
      }
      if (amp == std::string::npos) break;
      start = amp + 1;
    }
  }
  if (!r.bearer.empty()) req->addHeader("Authorization", "Bearer " + r.bearer);
  for (const auto& [k, v] : r.headers) req->addHeader(k, v);
  if (!r.body.empty() || r.method == "POST" || r.method == "PATCH" || r.method == "PUT") {
    req->setBody(r.body);
    req->setContentTypeString(r.contentType);
  }

  HttpResult out;
  auto [result, resp] = client->sendRequest(req, r.timeoutSec);
  if (result != drogon::ReqResult::Ok || !resp) return out;

  out.status = static_cast<int>(resp->statusCode());
  out.contentType = std::string(resp->getHeader("content-type"));
  out.body = std::string(resp->body());
  for (const auto& [k, v] : resp->headers()) out.headers.emplace_back(k, v);
  for (const auto& [k, c] : resp->cookies()) {
    auto line = c.cookieString();  // "Set-Cookie: name=value; ...\r\n"
    if (line.starts_with("Set-Cookie: ")) line = line.substr(12);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    out.setCookies.push_back(line);
  }
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errs;
  if (!reader->parse(out.body.data(), out.body.data() + out.body.size(), &out.json, &errs)) {
    out.json = Json::Value();
  }
  return out;
}

HttpResult httpGet(const std::string& url, double timeoutSec) {
  Request r;
  r.url = url;
  r.timeoutSec = timeoutSec;
  return send(r);
}

HttpResult sendJson(const std::string& method, const std::string& url, const Json::Value& body,
                    const std::string& bearer, const std::string& contentType) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  Request r;
  r.method = method;
  r.url = url;
  r.bearer = bearer;
  r.body = Json::writeString(w, body);
  r.contentType = contentType;
  return send(r);
}

std::string HttpResult::header(const std::string& name) const {
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
  };
  for (const auto& [k, v] : headers) {
    if (lower(k) == lower(name)) return v;
  }
  return {};
}

}  // namespace thelife::test
