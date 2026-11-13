#pragma once

#include <map>
#include <string>
#include <string_view>

namespace thelife::agent {

// A versioned prompt from prompts/ (agent_lld.md §9): front matter (id, version, ...) and a {{var}} template.
struct Prompt {
  std::string id;
  std::string version;
  std::string hash;  // first 12 hex of SHA-256 over the file, recorded with every message
  std::string body;

  // Replaces {{name}} with vars[name]; unknown placeholders throw (a prompt and its caller must agree).
  std::string render(const std::map<std::string, std::string>& vars) const;
};

Prompt parsePrompt(std::string_view text);

// prompts/<path>.md, compiled into the binary (for example "jobs/compress_day"). Throws for unknown paths.
const Prompt& prompt(const std::string& path);

// prompts/agent/system.md.
const Prompt& agentSystemPrompt();

}  // namespace thelife::agent
