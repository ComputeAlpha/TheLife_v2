#pragma once

#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

// Window classification (backend_lld.md §7.4): the user's rules first, then the built-in defaults
// (config/ingest_rules.json, compiled in). Pure functions; the derivation job loads the user's rules.
namespace thelife::ingest {

struct Rule {
  std::string matchWmClass, matchExe, matchTitle;  // empty = not part of the match
  std::string app, category, area;
  double confidence = 0.5;
  std::string parser;  // vscode | jetbrains | browser | dash_app | "" (none)
  int specificity() const { return !matchWmClass.empty() + !matchExe.empty() + !matchTitle.empty(); }
};

struct Window {
  std::string wmInstance, wmClass, title, exe;
};

struct Classification {
  std::string app, category, area, subject;
  double confidence = 0.2;
  bool matched = false;
};

// The built-in rules, most specific first.
const std::vector<Rule>& defaultRules();
// Parses {"rules": [...]} (config/ingest_rules.json format); throws std::invalid_argument on bad input.
std::vector<Rule> parseRules(const Json::Value& doc);
// One ingest_rules row (API shape) → Rule.
Rule ruleFromRow(const Json::Value& row);

bool matches(const Rule& r, const Window& w);
// `userRules` first (most specific first), then the defaults. Unmatched windows are "unknown"/other with low
// confidence and the window class as app.
Classification classify(const std::vector<Rule>& userRules, const Window& w);
// Subject from a title by parser name ("main.cc — TheLife_v2 — Visual Studio Code" → "TheLife_v2").
std::string parseSubject(const std::string& parser, const std::string& title);

}  // namespace thelife::ingest
