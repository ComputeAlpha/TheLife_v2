#pragma once

#include <json/value.h>

#include <string>
#include <string_view>

namespace thelife::json {

// Throws std::invalid_argument on malformed JSON. Rejects duplicate keys and limits depth.
Json::Value parse(std::string_view text);
std::string compact(const Json::Value& v);

}  // namespace thelife::json
