#include "http/web_root.h"

#include <drogon/drogon.h>

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace thelife::http {

namespace {

namespace fs = std::filesystem;

// Scripts only from the origin. Styles also allow inline <style> elements: dialogs lock page scrolling with a
// generated stylesheet whose content varies (the scrollbar width), so a hash cannot pin it.
constexpr const char* kCsp =
    "default-src 'self'; img-src 'self' data:; font-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self'; "
    "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'";

bool reserved(std::string_view p) {
  return p.starts_with("/api/") || p == "/api" || p == "/healthz" || p == "/readyz";
}

// Maps a request path to a file under root, or nullopt when it has no file (or tries to leave root).
std::optional<fs::path> resolve(const fs::path& root, std::string_view reqPath) {
  if (reqPath.empty() || reqPath.front() != '/') return std::nullopt;
  const fs::path rel(std::string(reqPath.substr(1)));
  for (const auto& part : rel) {
    const auto s = part.string();
    if (s == ".." || s == "." || (!s.empty() && s.front() == '.')) return std::nullopt;  // also hides dotfiles
  }
  std::error_code ec;
  const auto full = fs::weakly_canonical(root / rel, ec);
  if (ec) return std::nullopt;
  const auto rootStr = root.string();
  const auto fullStr = full.string();
  if (fullStr.size() <= rootStr.size() || fullStr.compare(0, rootStr.size(), rootStr) != 0 ||
      fullStr[rootStr.size()] != '/') {
    return std::nullopt;
  }
  if (!fs::is_regular_file(full, ec)) return std::nullopt;
  return full;
}

void secure(const drogon::HttpResponsePtr& r) {
  r->addHeader("Content-Security-Policy", kCsp);
  r->addHeader("X-Content-Type-Options", "nosniff");
  r->addHeader("Referrer-Policy", "no-referrer");
  r->addHeader("Cross-Origin-Opener-Policy", "same-origin");
  r->addHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
}

}  // namespace

void installWebRoot(const std::string& rootArg) {
  std::error_code ec;
  const auto root = fs::canonical(rootArg, ec);
  if (ec || !fs::is_regular_file(root / "index.html")) {
    throw std::runtime_error("THELIFE_WEB_ROOT=" + rootArg + " has no index.html (build the frontend first)");
  }
  const auto index = (root / "index.html").string();
  drogon::app().registerPreRoutingAdvice([root, index](const drogon::HttpRequestPtr& req,
                                                       drogon::AdviceCallback&& respond,
                                                       drogon::AdviceChainCallback&& next) {
    const auto m = req->method();
    if ((m != drogon::Get && m != drogon::Head) || reserved(req->path())) return next();
    const auto file = resolve(root, req->path());
    // A missing file with an extension is a real 404 (a stale asset URL), not a client-side route.
    if (!file && fs::path(req->path()).has_extension()) {
      auto r = drogon::HttpResponse::newHttpResponse();
      r->setStatusCode(drogon::k404NotFound);
      r->setContentTypeString("text/plain; charset=utf-8");
      r->setBody("Not found\n");
      secure(r);
      return respond(r);
    }
    auto r = drogon::HttpResponse::newFileResponse(file ? file->string() : index, "", drogon::CT_NONE, "", req);
    const bool immutable = file && req->path().starts_with("/assets/");
    r->addHeader("Cache-Control", immutable ? "public, max-age=31536000, immutable" : "no-cache");
    secure(r);
    respond(r);
  });
  LOG_INFO << "serving the web UI from " << root.string();
}

}  // namespace thelife::http
