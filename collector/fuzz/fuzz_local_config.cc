// libFuzzer target: config.toml parsing (a hand-edited file must never crash the collector).
#include <cstddef>
#include <cstdint>
#include <string>

#include "config/local_config.h"

using namespace thelife::collector;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string err;
  const auto c = parseLocalConfig(std::string(reinterpret_cast<const char*>(data), size), &err);
  if (!c) return 0;
  if (!validBackendUrl(c->backendUrl, &err)) __builtin_trap();
  // What enroll writes, run reads back the same.
  const auto again = parseLocalConfig(renderLocalConfig(*c), &err);
  if (!again || again->backendUrl != c->backendUrl || again->argsAllowlist != c->argsAllowlist) __builtin_trap();
  return 0;
}
