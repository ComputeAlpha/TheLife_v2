#pragma once

#include <json/value.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace thelife::tools {

enum class Effect { Read, Create, Update, Delete };

struct RestMapping {
  std::string method;  // GET, POST, PATCH, DELETE
  std::string path;    // /api/v1/tasks/{id}
  int successStatus = 200;
  std::vector<std::string> pathArgs, queryArgs, bodyArgs;
  bool ifMatch = false;
  std::string contentType;  // empty → application/json (merge-patch for PATCH)
};

struct ToolDef {
  std::string name;
  std::string description;
  Effect effect = Effect::Read;
  std::vector<std::string> scopes;
  std::set<std::string> exposure;    // rest, agent, mcp
  std::set<std::string> principals;  // user, agent, api_key, device
  int stage = 1;
  std::optional<std::string> targetResource;
  std::optional<std::string> policy;
  std::optional<std::string> agentGroup;  // agent-exposed tools: core | planning | work | ...
  Json::Value inputSchema;
  Json::Value outputSchema;
  std::optional<RestMapping> rest;
};

struct Manifest {
  std::string contractVersion;
  std::map<std::string, ToolDef> tools;
  std::map<std::string, std::vector<std::string>> agentGroups;  // group → tool names (docs/tool_registry.md §6)
};

// Parses the generated tools.json (api/generated/tools.json). Throws std::runtime_error on malformed input.
Manifest parseManifest(std::string_view json);
// The manifest compiled into this binary.
const Manifest& embeddedManifest();

}  // namespace thelife::tools
