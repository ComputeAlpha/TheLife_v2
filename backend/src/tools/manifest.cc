#include "tools/manifest.h"

#include <stdexcept>

#include "core/json.h"

namespace thelife::generated {
extern const std::string_view kToolsJson;
}

namespace thelife::tools {

namespace {
std::vector<std::string> strings(const Json::Value& v) {
  std::vector<std::string> out;
  for (const auto& s : v) out.push_back(s.asString());
  return out;
}

Effect parseEffect(const std::string& s) {
  if (s == "read") return Effect::Read;
  if (s == "create") return Effect::Create;
  if (s == "update") return Effect::Update;
  if (s == "delete") return Effect::Delete;
  throw std::runtime_error("unknown effect " + s);
}
}  // namespace

Manifest parseManifest(std::string_view text) {
  const auto doc = json::parse(text);
  Manifest m;
  m.contractVersion = doc["contract_version"].asString();
  for (const auto& t : doc["tools"]) {
    ToolDef d;
    d.name = t["name"].asString();
    d.description = t["description"].asString();
    d.effect = parseEffect(t["effect"].asString());
    d.scopes = strings(t["scopes"]);
    for (const auto& e : strings(t["exposure"])) d.exposure.insert(e);
    for (const auto& p : strings(t["principals"])) d.principals.insert(p);
    d.stage = t["stage"].asInt();
    if (t["target"].isObject()) d.targetResource = t["target"]["resource"].asString();
    if (t["policy"].isString()) d.policy = t["policy"].asString();
    if (t["agent_group"].isString()) d.agentGroup = t["agent_group"].asString();
    d.inputSchema = t["input_schema"];
    d.outputSchema = t["output_schema"];
    if (t["rest"].isObject()) {
      const auto& r = t["rest"];
      RestMapping rm;
      rm.method = r["method"].asString();
      rm.path = r["path"].asString();
      rm.successStatus = r["success_status"].asInt();
      rm.pathArgs = strings(r["path_args"]);
      rm.queryArgs = strings(r["query_args"]);
      rm.bodyArgs = strings(r["body_args"]);
      rm.ifMatch = r["if_match"].asBool();
      if (r["content_type"].isString()) rm.contentType = r["content_type"].asString();
      d.rest = rm;
    }
    m.tools.emplace(d.name, std::move(d));
  }
  for (const auto& g : doc["agent_groups"].getMemberNames()) m.agentGroups[g] = strings(doc["agent_groups"][g]);
  if (m.tools.empty()) throw std::runtime_error("tool manifest is empty");
  return m;
}

const Manifest& embeddedManifest() {
  static const Manifest m = parseManifest(generated::kToolsJson);
  return m;
}

}  // namespace thelife::tools
