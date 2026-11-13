#pragma once

#include <json/value.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Deterministic day score (scoring_lld.md §4, §6.2). Pure: everything it needs arrives in DayInputs, so golden
// tests drive it directly and recomputing with unchanged inputs gives the same hash and numbers.
namespace thelife::scoring {

inline constexpr const char* kFormulaVersion = "2";
// Areas scored from plan and targets; entertainment and other carry no weight (§1, decision G).
inline const std::vector<std::string>& scoredAreas() {
  static const std::vector<std::string> a{"work", "skill", "fitness", "personal"};
  return a;
}

// Evidence of what happened, as an interval. precedence: 0 user (activities, sessions, claims), 1 vision,
// 2 process, 3 external; per minute the lowest wins (backend_lld.md §6.5, decision E).
struct Evidence {
  int64_t from = 0, to = 0;  // unix seconds
  std::string area, category, linkType, linkId;
  int precedence = 0;
};

struct PlanItem {
  std::string type;  // task | block | deliverable
  std::string id, title, area;
  double weight = 1;
  double completion = 0;     // tasks and deliverables: given; blocks: computed from evidence
  int64_t from = 0, to = 0;  // blocks
  std::string linkType, linkId;  // a linked block counts only evidence linked to the same item
  bool due = false;          // due this day (while_due preemption)
  bool excluded = false;     // replanned before its start: shown, not scored (§4.1)
};

struct TargetInput {
  std::string id, title, area, metric;
  double amount = 0;
  std::optional<double> achieved;  // non-minute metrics come precomputed; minutes are taken from evidence
  std::string category, linkType, linkId;
  bool trackConsistency = false;
  int priorApplicable = 0, priorMet = 0;  // the previous 6 applicable days, for consistency
};

// A routine tracked for consistency: occurrences over the trailing 7 applicable days, today included.
struct TrackedRoutine {
  std::string id, title;
  int applicable = 0, completed = 0;
};

struct Settings {
  std::map<std::string, double> weights{{"work", 0.30}, {"skill", 0.25}, {"fitness", 0.15}, {"personal", 0.15},
                                        {"consistency", 0.15}};
  std::map<std::string, double> alpha{{"work", 0.8}, {"skill", 0.8}, {"fitness", 0.8}, {"personal", 0.8}};
};

struct DayInputs {
  std::string localDay, profile = "workday";
  bool scored = true;    // profile counts (leave and sick do not)
  bool observed = true;  // any evidence at all that day (§4.6)
  int64_t dayFrom = 0, dayTo = 0;
  std::vector<PlanItem> items;
  std::vector<Evidence> evidence;
  std::vector<TargetInput> targets;
  std::vector<TrackedRoutine> routines;
  Settings settings;
};

struct Preemption {
  std::string kind;  // overlap | while_due
  std::string displacedType, displacedId, displacedTitle;
  int minutes = 0;
};

struct DayResult {
  std::string status;  // final (before adjustment) | unobserved | not_scored
  std::optional<double> base;
  Json::Value breakdown, consistency;
  std::vector<Preemption> preemptions;
  std::map<std::string, int> areaMinutes;  // resolved evidence minutes per area
  std::string inputsHash;
};

DayResult scoreDay(const DayInputs& in);
// Progress so far (no score number): evidence and blocks up to `now`.
Json::Value progress(const DayInputs& in, int64_t now);
// final = clamp(round(base + 0.3 · pct), 0, 100) (§8): additive, so the adjustment can lift a 0-base day
// (decided 2026-11-13). pct is -30..30, so at most ±9 points.
inline constexpr double kAdjustmentPointsPerPct = 0.3;
double finalScore(double base, double adjustmentPct);
std::string hashInputs(const DayInputs& in);

}  // namespace thelife::scoring
