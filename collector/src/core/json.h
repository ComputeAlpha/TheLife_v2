#pragma once

#include <json/json.h>

#include <optional>
#include <string>
#include <string_view>

namespace thelife::collector::json {

// Everything the backend sends is parsed through this (collector_lld.md §8): bounded size and nesting depth,
// strict syntax, duplicate keys rejected. Returns nullopt instead of throwing.
constexpr size_t kMaxResponseBytes = 256 * 1024;
constexpr int kMaxDepth = 16;
std::optional<Json::Value> parseStrict(std::string_view text, size_t maxBytes = kMaxResponseBytes);

std::string compact(const Json::Value& v);

// Truncates to at most `maxBytes` bytes without splitting a UTF-8 sequence, and replaces invalid bytes.
std::string utf8Truncate(std::string_view s, size_t maxBytes);

}  // namespace thelife::collector::json
