#include "config/local_config.h"

#include <toml++/toml.hpp>

#include <sstream>

#include "core/files.h"
#include "core/log.h"

namespace thelife::collector {

namespace {

struct UrlParts {
  std::string scheme, host, rest;
};

std::optional<UrlParts> splitUrl(const std::string& url) {
  const auto p = url.find("://");
  if (p == std::string::npos) return std::nullopt;
  UrlParts u;
  u.scheme = url.substr(0, p);
  auto hostEnd = url.find('/', p + 3);
  std::string authority = url.substr(p + 3, hostEnd == std::string::npos ? std::string::npos : hostEnd - p - 3);
  u.rest = hostEnd == std::string::npos ? "" : url.substr(hostEnd);
  if (authority.empty() || authority.find('@') != std::string::npos) return std::nullopt;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos) return std::nullopt;
    u.host = authority.substr(0, close + 1);
  } else {
    u.host = authority.substr(0, authority.find(':'));
  }
  return u;
}

}  // namespace

bool isLoopbackUrl(const std::string& url) {
  const auto u = splitUrl(url);
  return u && (u->host == "127.0.0.1" || u->host == "localhost" || u->host == "[::1]");
}

bool validBackendUrl(const std::string& url, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  const auto u = splitUrl(url);
  if (!u || u->host.empty()) return fail("not a URL (expected https://host[:port][/prefix])");
  for (char c : url) {
    if (c <= ' ' || c == '?' || c == '#' || c == '\\' || c == '"' || c == 127) {
      return fail("URL must not contain spaces, a query or a fragment");
    }
  }
  if (u->scheme == "https") return true;
  if (u->scheme == "http") {
    if (isLoopbackUrl(url)) return true;
    return fail("plain http is only allowed for 127.0.0.1, localhost or [::1]; use https");
  }
  return fail("scheme must be https (or http for loopback)");
}

std::optional<LocalConfig> parseLocalConfig(const std::string& text, std::string* err) {
  toml::table t;
  try {
    t = toml::parse(text);
  } catch (const toml::parse_error& e) {
    if (err) *err = std::string("config.toml: ") + std::string(e.description());
    return std::nullopt;
  }
  LocalConfig c;
  c.backendUrl = t["backend_url"].value_or(std::string());
  while (!c.backendUrl.empty() && c.backendUrl.back() == '/') c.backendUrl.pop_back();
  if (!validBackendUrl(c.backendUrl, err)) {
    if (err) *err = "backend_url: " + *err;
    return std::nullopt;
  }
  c.caFile = t["ca_file"].value_or(std::string());
  c.pinSha256 = t["pin_sha256"].value_or(std::string());
  if (!c.pinSha256.empty() && !c.pinSha256.starts_with("sha256//")) {
    if (err) *err = "pin_sha256 must look like sha256//<base64>";
    return std::nullopt;
  }
  c.logLevel = t["log_level"].value_or(std::string("info"));
  log::Level lvl;
  if (!log::parseLevel(c.logLevel, lvl)) {
    if (err) *err = "log_level must be debug, info, warn or error";
    return std::nullopt;
  }
  c.webUiUrl = t["web_ui_url"].value_or(std::string());
  while (!c.webUiUrl.empty() && c.webUiUrl.back() == '/') c.webUiUrl.pop_back();
  if (!c.webUiUrl.empty()) {
    // Opened in the user's browser on a click; any http(s) host is fine, but nothing beyond origin and path.
    const auto u = splitUrl(c.webUiUrl);
    bool ok = u && !u->host.empty() && (u->scheme == "http" || u->scheme == "https");
    for (char ch : c.webUiUrl) ok = ok && !(ch <= ' ' || ch == '?' || ch == '#' || ch == '"' || ch == '\\' || ch == 127);
    if (!ok) {
      if (err) *err = "web_ui_url must be an http(s) URL without a query or fragment";
      return std::nullopt;
    }
  }
  if (const auto* arr = t["args_allowlist"].as_array()) {
    for (const auto& v : *arr) {
      const auto s = v.value<std::string>();
      if (!s || s->empty() || s->find('/') != std::string::npos) {
        if (err) *err = "args_allowlist holds executable basenames (no paths)";
        return std::nullopt;
      }
      c.argsAllowlist.push_back(*s);
    }
  }
  return c;
}

std::string renderLocalConfig(const LocalConfig& c) {
  toml::table t;
  t.insert("backend_url", c.backendUrl);
  if (!c.webUiUrl.empty()) t.insert("web_ui_url", c.webUiUrl);
  if (!c.caFile.empty()) t.insert("ca_file", c.caFile);
  if (!c.pinSha256.empty()) t.insert("pin_sha256", c.pinSha256);
  t.insert("log_level", c.logLevel);
  toml::array allow;
  for (const auto& a : c.argsAllowlist) allow.push_back(a);
  t.insert("args_allowlist", allow);
  std::ostringstream out;
  out << "# TheLife collector (collector_lld.md §4.1). The device token lives in device.token, never here.\n"
      << t << "\n";
  return out.str();
}

std::string configPath() { return files::configDir() + "/config.toml"; }
std::string tokenPath() { return files::configDir() + "/device.token"; }

}  // namespace thelife::collector
