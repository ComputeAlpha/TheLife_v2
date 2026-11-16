#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace thelife::collector::log {

namespace {
std::atomic<Level> gLevel{Level::Info};
std::mutex gMu;
}  // namespace

void setLevel(Level l) { gLevel = l; }

bool parseLevel(std::string_view s, Level& out) {
  if (s == "debug") out = Level::Debug;
  else if (s == "info") out = Level::Info;
  else if (s == "warn") out = Level::Warn;
  else if (s == "error") out = Level::Error;
  else return false;
  return true;
}

void write(Level l, std::string_view msg) {
  if (l < gLevel.load()) return;
  // sd-daemon priority prefixes, so journald records the level.
  static constexpr const char* kPrefix[] = {"<7>", "<6>", "<4>", "<3>"};
  std::lock_guard lock(gMu);
  std::fprintf(stderr, "%s%.*s\n", kPrefix[static_cast<int>(l)], static_cast<int>(msg.size()), msg.data());
  std::fflush(stderr);
}

}  // namespace thelife::collector::log
