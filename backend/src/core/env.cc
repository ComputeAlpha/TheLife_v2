#include "core/env.h"

#include <cstdlib>
#include <stdexcept>

namespace thelife::env {

std::string get(std::string_view name, std::string_view fallback) {
  const char* v = std::getenv(std::string(name).c_str());
  if (v == nullptr || *v == '\0') return std::string(fallback);
  return v;
}

int getInt(std::string_view name, int fallback, int min, int max) {
  const std::string raw = get(name);
  if (raw.empty()) return fallback;
  std::size_t used = 0;
  int value = 0;
  try {
    value = std::stoi(raw, &used);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) + " is not an integer");
  }
  if (used != raw.size()) throw std::invalid_argument(std::string(name) + " is not an integer");
  if (value < min || value > max) throw std::invalid_argument(std::string(name) + " is out of range");
  return value;
}

}  // namespace thelife::env
