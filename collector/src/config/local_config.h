#pragma once

#include <optional>
#include <string>
#include <vector>

namespace thelife::collector {

// ~/.config/thelife-collector/config.toml (collector_lld.md §4.1). Only local values: the server can change none
// of these.
struct LocalConfig {
  std::string backendUrl;  // origin, optionally with a path prefix; the API lives under <backendUrl>/api/v1
  std::string caFile;      // optional private CA bundle
  std::string pinSha256;   // optional "sha256//<base64>" public-key pin
  std::string logLevel = "info";
  std::string webUiUrl;  // optional; notification clicks open <webUiUrl>/chat
  // Executable basenames whose arguments may be sent in process snapshots (collector_lld.md §5.3).
  std::vector<std::string> argsAllowlist;
};

// https:// any host, or http:// only for loopback (127.0.0.1, localhost, [::1]). No credentials, query or fragment.
bool validBackendUrl(const std::string& url, std::string* err);
bool isLoopbackUrl(const std::string& url);

std::optional<LocalConfig> parseLocalConfig(const std::string& toml, std::string* err);
std::string renderLocalConfig(const LocalConfig& c);

std::string configPath();  // <configDir>/config.toml
std::string tokenPath();   // <configDir>/device.token

}  // namespace thelife::collector
