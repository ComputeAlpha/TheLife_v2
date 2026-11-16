#pragma once

#include <json/json.h>

#include <optional>
#include <string>

#include "core/time.h"

namespace thelife::collector {

// ~/.local/state/thelife-collector/state.json: what `thelife-collector status` prints (collector_lld.md §2).
// Written by the daemon only; the pause lives in the queue database so the `pause` command and the daemon never
// write the same file.
std::string statePath();
std::string queuePath();
bool writeState(const Json::Value& s);
std::optional<Json::Value> readState();

// The local privacy pause (collector_lld.md §2), stored as queue metadata.
constexpr const char* kMetaPausedUntil = "paused_until";
constexpr const char* kMetaServerConfig = "server_config";
constexpr const char* kMetaConfigEtag = "server_config_etag";
constexpr const char* kMetaTokenIssuedAt = "token_issued_at";
constexpr const char* kMetaInstallId = "install_id";

}  // namespace thelife::collector
