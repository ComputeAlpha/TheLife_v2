#include "ingest/rules.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "core/json.h"

namespace thelife::generated {
extern const std::string_view kIngestRulesJson;
}

namespace thelife::ingest {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool equalsOrPrefix(const std::string& pattern, const std::string& value) {
  if (pattern.empty() || value.empty()) return false;
  const auto p = lower(pattern), v = lower(value);
  if (p.back() == '*') return v.rfind(p.substr(0, p.size() - 1), 0) == 0;
  return p == v;
}

std::string baseName(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string trim(std::string s) {
  // Also the "unsaved" markers editors put in front ("● main.cc", "*main.cc").
  for (const std::string mark : {"\xe2\x97\x8f", "*"}) {
    const auto b = s.find_first_not_of(" \t");
    if (b != std::string::npos && s.compare(b, mark.size(), mark) == 0) s.erase(0, b + mark.size());
  }
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// Splits on " — ", " – " and " - " (editors and browsers use any of them).
std::vector<std::string> segments(const std::string& title) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (true) {
    std::size_t best = std::string::npos, len = 0;
    for (const std::string sep : {" \xe2\x80\x94 ", " \xe2\x80\x93 ", " - "}) {
      const auto f = title.find(sep, pos);
      if (f < best) best = f, len = sep.size();
    }
    if (best == std::string::npos) {
      out.push_back(trim(title.substr(pos)));
      return out;
    }
    out.push_back(trim(title.substr(pos, best - pos)));
    pos = best + len;
  }
}

std::string cap(std::string s, std::size_t n = 120) {
  if (s.size() <= n) return s;
  s.resize(n);
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80)) s.pop_back();
  return s;
}

std::vector<Rule> sortedBySpecificity(std::vector<Rule> rules) {
  std::stable_sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) { return a.specificity() > b.specificity(); });
  return rules;
}

}  // namespace

std::vector<Rule> parseRules(const Json::Value& doc) {
  std::vector<Rule> out;
  if (!doc["rules"].isArray()) throw std::invalid_argument("ingest rules: \"rules\" must be an array");
  for (const auto& r : doc["rules"]) {
    Rule rule = ruleFromRow(r);
    rule.parser = r.get("parser", "").asString();
    static const std::vector<std::string> kParsers{"", "vscode", "jetbrains", "browser", "dash_app"};
    if (std::find(kParsers.begin(), kParsers.end(), rule.parser) == kParsers.end()) {
      throw std::invalid_argument("ingest rules: unknown parser " + rule.parser);
    }
    if (rule.specificity() == 0 || rule.app.empty() || rule.category.empty() || rule.area.empty()) {
      throw std::invalid_argument("ingest rules: a rule needs a match field, app, category and area");
    }
    out.push_back(std::move(rule));
  }
  return sortedBySpecificity(std::move(out));
}

Rule ruleFromRow(const Json::Value& row) {
  auto str = [&](const char* k) { return row[k].isString() ? row[k].asString() : std::string(); };
  Rule r;
  r.matchWmClass = str("match_wm_class");
  r.matchExe = str("match_exe");
  r.matchTitle = lower(str("match_title"));
  r.app = str("app");
  r.category = str("category");
  r.area = str("area");
  r.confidence = row.get("confidence", 0.5).asDouble();
  return r;
}

const std::vector<Rule>& defaultRules() {
  static const std::vector<Rule> rules = parseRules(json::parse(generated::kIngestRulesJson));
  return rules;
}

bool matches(const Rule& r, const Window& w) {
  if (r.specificity() == 0) return false;
  if (!r.matchWmClass.empty() && !equalsOrPrefix(r.matchWmClass, w.wmClass) &&
      !equalsOrPrefix(r.matchWmClass, w.wmInstance)) {
    return false;
  }
  if (!r.matchExe.empty() && !equalsOrPrefix(r.matchExe, w.exe) && !equalsOrPrefix(r.matchExe, baseName(w.exe))) {
    return false;
  }
  if (!r.matchTitle.empty() && lower(w.title).find(r.matchTitle) == std::string::npos) return false;
  return true;
}

std::string parseSubject(const std::string& parser, const std::string& title) {
  if (parser.empty() || title.empty()) return "";
  auto seg = segments(title);
  if (parser == "vscode") {
    // "file — folder — Visual Studio Code" (or "folder — Visual Studio Code" with nothing open)
    if (seg.size() >= 3) return cap(seg[seg.size() - 2]);
    if (seg.size() == 2) return cap(seg[0]);
    return "";
  }
  if (parser == "jetbrains") return seg.size() >= 2 ? cap(seg[0]) : "";  // "project – file"
  if (parser == "browser" || parser == "dash_app") {
    // "page — Mozilla Firefox", "report.odt - LibreOffice Writer": everything before the app suffix.
    if (seg.size() < 2) return cap(seg.empty() ? "" : seg[0]);
    std::string s;
    for (std::size_t i = 0; i + 1 < seg.size(); ++i) s += (i ? " - " : "") + seg[i];
    return cap(s);
  }
  return "";
}

Classification classify(const std::vector<Rule>& userRules, const Window& w) {
  const auto pick = [&](const std::vector<Rule>& rules) -> const Rule* {
    for (const auto& r : rules) {
      if (matches(r, w)) return &r;
    }
    return nullptr;
  };
  const Rule* r = pick(sortedBySpecificity(userRules));
  if (!r) r = pick(defaultRules());
  Classification c;
  if (!r) {
    c.app = !w.wmClass.empty() ? w.wmClass : !w.exe.empty() ? baseName(w.exe) : "unknown";
    c.category = "unknown";
    c.area = "other";
    c.confidence = 0.2;
    c.subject = parseSubject("dash_app", w.title);
    return c;
  }
  c.app = r->app;
  c.category = r->category;
  c.area = r->area;
  c.confidence = r->confidence;
  c.matched = true;
  // A user rule has no parser of its own: borrow the default rule's for the same window, if any.
  std::string parser = r->parser;
  if (parser.empty()) {
    if (const Rule* d = pick(defaultRules())) parser = d->parser;
  }
  c.subject = parseSubject(parser, w.title);
  return c;
}

}  // namespace thelife::ingest
