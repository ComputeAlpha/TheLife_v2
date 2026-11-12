#pragma once

#include <string>
#include <string_view>

namespace thelife::env {

// Returns the environment variable or `fallback` when unset or empty.
std::string get(std::string_view name, std::string_view fallback = {});

// Parses an integer environment variable; throws std::invalid_argument on garbage or out-of-range values.
int getInt(std::string_view name, int fallback, int min, int max);

}  // namespace thelife::env
