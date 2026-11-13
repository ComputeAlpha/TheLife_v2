#include "agent/prompt.h"

#include <stdexcept>

#include "core/crypto.h"

namespace thelife::generated {
const std::map<std::string, std::string_view>& promptFiles();
}

namespace thelife::agent {

Prompt parsePrompt(std::string_view text) {
  Prompt p;
  const auto digest = crypto::sha256(text);
  static constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < 6 && i < digest.size(); ++i) {
    const auto c = static_cast<unsigned char>(digest[i]);
    p.hash += kHex[c >> 4];
    p.hash += kHex[c & 0x0F];
  }
  if (text.rfind("---\n", 0) != 0) throw std::runtime_error("prompt without front matter");
  const auto end = text.find("\n---\n", 4);
  if (end == std::string_view::npos) throw std::runtime_error("prompt front matter not closed");
  std::string_view fm = text.substr(4, end - 4);
  while (!fm.empty()) {
    const auto nl = fm.find('\n');
    const auto line = fm.substr(0, nl);
    fm = nl == std::string_view::npos ? std::string_view() : fm.substr(nl + 1);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const auto key = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    if (key == "id") p.id = value;
    if (key == "version") p.version = value;
  }
  if (p.id.empty() || p.version.empty()) throw std::runtime_error("prompt front matter needs id and version");
  p.body = std::string(text.substr(end + 5));
  return p;
}

std::string Prompt::render(const std::map<std::string, std::string>& vars) const {
  std::string out;
  std::size_t pos = 0;
  while (true) {
    const auto open = body.find("{{", pos);
    if (open == std::string::npos) break;
    const auto close = body.find("}}", open);
    if (close == std::string::npos) break;
    out.append(body, pos, open - pos);
    const auto name = body.substr(open + 2, close - open - 2);
    const auto it = vars.find(name);
    if (it == vars.end()) throw std::runtime_error("prompt " + id + ": no value for {{" + name + "}}");
    out += it->second;
    pos = close + 2;
  }
  out.append(body, pos);
  return out;
}

const Prompt& prompt(const std::string& path) {
  static const std::map<std::string, Prompt> all = [] {
    std::map<std::string, Prompt> m;
    for (const auto& [name, text] : generated::promptFiles()) m.emplace(name, parsePrompt(text));
    return m;
  }();
  const auto it = all.find(path);
  if (it == all.end()) throw std::runtime_error("no prompt " + path);
  return it->second;
}

const Prompt& agentSystemPrompt() { return prompt("agent/system"); }

}  // namespace thelife::agent
