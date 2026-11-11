#include "support/http.h"

#include <drogon/HttpClient.h>
#include <json/reader.h>
#include <trantor/net/EventLoopThread.h>

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

HttpResult httpGet(const std::string& url, double timeoutSec) {
  std::string origin, path;
  splitUrl(url, origin, path);
  auto client = drogon::HttpClient::newHttpClient(origin, clientLoop());
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath(path);

  HttpResult out;
  auto [result, resp] = client->sendRequest(req, timeoutSec);
  if (result != drogon::ReqResult::Ok || !resp) return out;

  out.status = static_cast<int>(resp->statusCode());
  out.contentType = std::string(resp->getHeader("content-type"));
  out.body = std::string(resp->body());
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errs;
  if (!reader->parse(out.body.data(), out.body.data() + out.body.size(), &out.json, &errs)) {
    out.json = Json::Value();
  }
  return out;
}

}  // namespace thelife::test
