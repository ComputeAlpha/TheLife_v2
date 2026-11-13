// Scoring engine golden and property tests (scoring_lld.md §12). Expected numbers are worked out by hand from the
// formulas in §4; two decimals as stored.
#include <gtest/gtest.h>

#include <random>

#include "scoring/engine.h"

using namespace thelife::scoring;

namespace {

constexpr int64_t D = 1794600000;  // a local day's start, as an instant
int64_t at(int hour, int minute = 0) { return D + hour * 3600 + minute * 60; }

DayInputs day() {
  DayInputs in;
  in.localDay = "2026-11-14";
  in.dayFrom = D;
  in.dayTo = D + 86400;
  return in;
}

PlanItem task(const char* id, const char* area, double weight, double completion, bool due = true) {
  PlanItem i;
  i.type = "task", i.id = id, i.title = id, i.area = area, i.weight = weight, i.completion = completion, i.due = due;
  return i;
}
PlanItem block(const char* id, const char* area, int64_t from, int64_t to) {
  PlanItem i;
  i.type = "block", i.id = id, i.title = id, i.area = area, i.from = from, i.to = to;
  i.weight = std::max(0.5, static_cast<double>(to - from) / 3600.0);
  return i;
}
Evidence ev(const char* area, int64_t from, int64_t to, int precedence = 0) {
  Evidence e;
  e.area = area, e.from = from, e.to = to, e.precedence = precedence;
  return e;
}
TargetInput target(const char* id, const char* area, const char* metric, double amount,
                   std::optional<double> achieved = std::nullopt) {
  TargetInput t;
  t.id = id, t.title = id, t.area = area, t.metric = metric, t.amount = amount, t.achieved = achieved;
  return t;
}

}  // namespace

TEST(ScoringGolden, Workday) {
  auto in = day();
  in.items = {task("ship", "work", 2, 1), task("email", "work", 1.5, 0), block("gym", "fitness", at(7), at(8))};
  in.evidence = {ev("fitness", at(7), at(7, 45))};
  in.targets = {target("read", "skill", "pages", 20, 25)};
  in.routines = {{"stretch", "Stretch", 7, 5}};
  const auto r = scoreDay(in);
  ASSERT_EQ(r.status, "final");
  const auto& a = r.breakdown["areas"];
  EXPECT_DOUBLE_EQ(a["work"]["score"].asDouble(), 57.14);    // (2·1 + 1.5·0) / 3.5
  EXPECT_DOUBLE_EQ(a["fitness"]["score"].asDouble(), 75.0);  // 45 of 60 block minutes
  EXPECT_DOUBLE_EQ(a["skill"]["score"].asDouble(), 100.0);   // target only: min(100, 100 + bonus 5)
  EXPECT_DOUBLE_EQ(a["skill"]["bonus"].asDouble(), 5.0);     // 25 % over → 6.25, capped at 5
  EXPECT_FALSE(a["personal"]["applicable"].asBool());
  EXPECT_DOUBLE_EQ(r.consistency["score"].asDouble(), 71.43);
  // (0.30·57.142857 + 0.25·100 + 0.15·75 + 0.15·71.428571) / 0.85
  EXPECT_DOUBLE_EQ(*r.base, 75.42);
}

TEST(ScoringGolden, WeekendKeepsConsistentItemsAndSkipsAbsentAreas) {
  auto in = day();
  in.profile = "weekend";
  in.targets = {target("read", "skill", "pages", 20, 10)};
  in.routines = {{"read-daily", "Read", 7, 7}};
  const auto r = scoreDay(in);
  EXPECT_FALSE(r.breakdown["areas"]["work"]["applicable"].asBool());  // no planned work: not a zero
  EXPECT_DOUBLE_EQ(*r.base, 68.75);  // (0.25·50 + 0.15·100) / 0.40
}

TEST(ScoringGolden, LeaveAndUnobservedDaysHaveNoBase) {
  auto leave = day();
  leave.profile = "leave";
  leave.scored = false;
  leave.items = {task("x", "work", 1, 0)};
  const auto l = scoreDay(leave);
  EXPECT_EQ(l.status, "not_scored");
  EXPECT_FALSE(l.base);

  auto silent = day();
  silent.observed = false;
  silent.items = {task("x", "work", 1, 0)};
  const auto u = scoreDay(silent);
  EXPECT_EQ(u.status, "unobserved");
  EXPECT_FALSE(u.base);

  auto empty = day();
  EXPECT_EQ(scoreDay(empty).status, "not_scored");  // nothing planned, no targets
}

TEST(ScoringGolden, SelfReportedWorkCountsLikeObservedWork) {
  auto in = day();
  in.targets = {target("workday", "work", "minutes", 360)};
  in.evidence = {ev("work", at(9), at(14))};  // a 5 h work log from chat
  const auto r = scoreDay(in);
  EXPECT_DOUBLE_EQ(r.breakdown["areas"]["work"]["targets"][0]["achieved"].asDouble(), 300);
  EXPECT_DOUBLE_EQ(*r.base, 83.33);
}

TEST(ScoringGolden, ReplannedBeforeStartIsNotAFailure) {
  auto in = day();
  auto moved = task("moved", "work", 3, 0);
  moved.excluded = true;
  in.items = {moved, task("done", "work", 1, 1)};
  const auto r = scoreDay(in);
  EXPECT_DOUBLE_EQ(r.breakdown["areas"]["work"]["adherence"].asDouble(), 1.0);
  EXPECT_TRUE(r.breakdown["areas"]["work"]["items"][0]["excluded"].asBool());
}

TEST(ScoringGolden, OverlapPrecedenceAndPreemption) {
  auto in = day();
  in.items = {block("deep-work", "work", at(9), at(10))};
  // The process detector saw the IDE all hour, but the user said the second half was gaming: user evidence wins.
  in.evidence = {ev("work", at(9), at(10), 2), ev("entertainment", at(9, 30), at(10), 0)};
  const auto r = scoreDay(in);
  EXPECT_DOUBLE_EQ(r.breakdown["areas"]["work"]["items"][0]["completion"].asDouble(), 0.5);
  ASSERT_EQ(r.preemptions.size(), 1u);
  EXPECT_EQ(r.preemptions[0].kind, "overlap");
  EXPECT_EQ(r.preemptions[0].minutes, 30);
  EXPECT_EQ(r.areaMinutes.at("work"), 30);  // the same minute never counts twice

  auto due = day();
  due.items = {task("report", "work", 2, 0)};
  due.evidence = {ev("entertainment", at(20), at(21))};
  const auto d = scoreDay(due);
  ASSERT_EQ(d.preemptions.size(), 1u);
  EXPECT_EQ(d.preemptions[0].kind, "while_due");
  EXPECT_EQ(d.preemptions[0].minutes, 60);
}

TEST(ScoringGolden, BonusIsCappedAndTheAreaAt100) {
  auto in = day();
  in.items = {task("t", "skill", 1, 1)};
  in.targets = {target("pages", "skill", "pages", 10, 14)};  // 40 % over
  const auto r = scoreDay(in);
  EXPECT_DOUBLE_EQ(r.breakdown["areas"]["skill"]["bonus"].asDouble(), 5.0);
  EXPECT_DOUBLE_EQ(r.breakdown["areas"]["skill"]["score"].asDouble(), 100.0);

  auto partial = day();
  partial.items = {task("a", "skill", 1, 1), task("b", "skill", 1, 0)};
  partial.targets = {target("pages", "skill", "pages", 10, 12)};  // 20 % over → +5
  // 100 · (0.8 · 0.5 + 0.2 · 1) + 5
  EXPECT_DOUBLE_EQ(scoreDay(partial).breakdown["areas"]["skill"]["score"].asDouble(), 65.0);
}

TEST(ScoringGolden, LinkedBlocksNeedLinkedEvidence) {
  auto in = day();
  auto b = block("rust", "skill", at(18), at(19));
  b.linkType = "skill_resource", b.linkId = "course-1";
  in.items = {b};
  auto other = ev("skill", at(18), at(18, 30));
  other.linkType = "skill_resource", other.linkId = "book-2";
  auto right = ev("skill", at(18, 30), at(19));
  right.linkType = "skill_resource", right.linkId = "course-1";
  in.evidence = {other, right};
  EXPECT_DOUBLE_EQ(scoreDay(in).breakdown["areas"]["skill"]["items"][0]["completion"].asDouble(), 0.5);
}

TEST(ScoringGolden, ConsistencyTargetsCountToday) {
  auto in = day();
  auto t = target("walk", "fitness", "minutes", 30);
  t.trackConsistency = true;
  t.priorApplicable = 6, t.priorMet = 3;
  in.targets = {t};
  in.evidence = {ev("fitness", at(6), at(6, 40))};
  const auto r = scoreDay(in);
  EXPECT_DOUBLE_EQ(r.consistency["items"][0]["rate"].asDouble(), 0.57);  // (3 + 1) / 7
}

TEST(ScoringFinal, AdjustmentIsClampedAndRounded) {
  EXPECT_DOUBLE_EQ(finalScore(75.42, 10), 78);  // + 3 points
  EXPECT_DOUBLE_EQ(finalScore(90, 30), 99);
  EXPECT_DOUBLE_EQ(finalScore(98, 30), 100);    // clamped
  EXPECT_DOUBLE_EQ(finalScore(10, -30), 1);
  EXPECT_DOUBLE_EQ(finalScore(5, -30), 0);      // clamped
  EXPECT_DOUBLE_EQ(finalScore(0, 25), 8);       // a 0-base day can be lifted (round(7.5))
}

TEST(ScoringProperties, BoundsMonotonicityApplicabilityAndIdempotence) {
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> u(0, 1);
  const char* areas[] = {"work", "skill", "fitness", "personal"};
  for (int n = 0; n < 300; ++n) {
    auto in = day();
    for (int i = 0; i < 6; ++i) {
      in.items.push_back(task(("t" + std::to_string(i)).c_str(), areas[rng() % 4], static_cast<double>(1 + rng() % 3), u(rng) < 0.5 ? 0 : 1));
    }
    in.targets = {target("m", areas[rng() % 4], "pages", 10, u(rng) * 20)};
    in.routines = {{"r", "r", 7, static_cast<int>(rng() % 8)}};
    const auto r = scoreDay(in);
    ASSERT_TRUE(r.base);
    EXPECT_GE(*r.base, 0);
    EXPECT_LE(*r.base, 100);

    // Completing an item never lowers the base.
    for (auto& i : in.items) {
      if (i.completion < 1) {
        i.completion = 1;
        break;
      }
    }
    EXPECT_GE(*scoreDay(in).base + 1e-9, *r.base);

    // Same inputs, same hash; changed inputs, another hash.
    EXPECT_EQ(hashInputs(in), hashInputs(in));
    auto changed = in;
    changed.items[0].weight += 1;
    EXPECT_NE(hashInputs(changed), hashInputs(in));
  }
  // An area with nothing applicable never changes the base, whatever its weight.
  auto in = day();
  in.items = {task("a", "work", 1, 1), task("b", "skill", 1, 0)};
  const auto before = *scoreDay(in).base;
  in.settings.weights["personal"] = 0.9;
  EXPECT_DOUBLE_EQ(*scoreDay(in).base, before);
}

TEST(ScoringProgress, CountsOnlyWhatHasStarted) {
  auto in = day();
  in.items = {block("am", "work", at(9), at(11)), block("pm", "work", at(14), at(16)), task("t", "work", 1, 1)};
  in.evidence = {ev("work", at(9), at(10))};
  const auto p = progress(in, at(10, 30));
  const auto& w = p["areas"]["work"];
  EXPECT_EQ(w["block_minutes_so_far"].asInt(), 90);
  EXPECT_EQ(w["block_minutes_covered"].asInt(), 60);
  EXPECT_EQ(w["done_items"].asInt(), 1);
}
