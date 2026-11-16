#include "app/state.h"

#include "core/files.h"
#include "core/json.h"

namespace thelife::collector {

std::string statePath() { return files::stateDir() + "/state.json"; }
std::string queuePath() { return files::stateDir() + "/queue.db"; }

bool writeState(const Json::Value& s) {
  std::string err;
  return files::writePrivateAtomic(statePath(), json::compact(s) + "\n", &err);
}

std::optional<Json::Value> readState() {
  std::string err;
  const auto text = files::readPrivate(statePath(), &err);
  if (!text) return std::nullopt;
  return json::parseStrict(*text);
}

}  // namespace thelife::collector
