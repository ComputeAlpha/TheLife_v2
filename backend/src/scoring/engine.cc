#include "scoring/engine.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "core/crypto.h"
#include "core/json.h"

namespace thelife::scoring {

namespace {

double round2(double v) { return std::round(v * 100.0) / 100.0; }

// Per minute of [from, to), the index of the winning evidence (or -1).
std::vector<int> resolveMinutes(const DayInputs& in, int64_t until) {
  const int64_t end = std::min(in.dayTo, until);
  const auto n = static_cast<std::size_t>(std::max<int64_t>(0, (end - in.dayFrom) / 60));
  std::vector<int> owner(n, -1);
  for (std::size_t e = 0; e < in.evidence.size(); ++e) {
    const auto& ev = in.evidence[e];
    const int64_t a = std::max(ev.from, in.dayFrom), b = std::min(ev.to, end);
    if (b <= a) continue;
    for (auto m = static_cast<std::size_t>((a - in.dayFrom) / 60);
         m < n && in.dayFrom + static_cast<int64_t>(m) * 60 < b; ++m) {
      const int cur = owner[m];
      if (cur < 0 || ev.precedence < in.evidence[static_cast<std::size_t>(cur)].precedence) owner[m] = static_cast<int>(e);
    }
  }
  return owner;
}

bool blockMatches(const PlanItem& block, const Evidence& ev) {
  if (ev.area != block.area) return false;
  if (block.linkType.empty()) return true;
  return ev.linkType == block.linkType && ev.linkId == block.linkId;
}

bool targetMatches(const TargetInput& t, const Evidence& ev) {
  if (ev.area != t.area) return false;
  if (!t.category.empty() && ev.category != t.category) return false;
  if (!t.linkType.empty() && (ev.linkType != t.linkType || ev.linkId != t.linkId)) return false;
  return true;
}

// Minutes of [from, to) whose winning evidence satisfies pred.
template <typename P>
int minutesIn(const DayInputs& in, const std::vector<int>& owner, int64_t from, int64_t to, P pred) {
  int count = 0;
  for (std::size_t m = 0; m < owner.size(); ++m) {
    const int64_t t = in.dayFrom + static_cast<int64_t>(m) * 60;
    if (t < from || t >= to || owner[m] < 0) continue;
    if (pred(in.evidence[static_cast<std::size_t>(owner[m])])) ++count;
  }
  return count;
}

double blockCompletion(const DayInputs& in, const std::vector<int>& owner, const PlanItem& b, int64_t until) {
  const int64_t from = std::max(b.from, in.dayFrom), to = std::min({b.to, in.dayTo, until});
  const double planned = static_cast<double>(std::max<int64_t>(0, to - from)) / 60.0;
  if (planned <= 0) return 0;
  const int covered = minutesIn(in, owner, from, to, [&](const Evidence& e) { return blockMatches(b, e); });
  return std::min(1.0, covered / planned);
}

}  // namespace

double finalScore(double base, double pct) {
  return std::clamp(std::round(base + kAdjustmentPointsPerPct * pct), 0.0, 100.0);
}

std::string hashInputs(const DayInputs& in) {
  Json::Value v;
  v["formula"] = kFormulaVersion;
  v["day"] = in.localDay;
  v["profile"] = in.profile;
  v["scored"] = in.scored;
  v["observed"] = in.observed;
  for (const auto& i : in.items) {
    Json::Value j;
    j["t"] = i.type, j["id"] = i.id, j["a"] = i.area, j["w"] = i.weight, j["c"] = i.completion;
    j["f"] = static_cast<Json::Int64>(i.from), j["to"] = static_cast<Json::Int64>(i.to), j["x"] = i.excluded;
    j["l"] = i.linkType + i.linkId;
    v["items"].append(j);
  }
  for (const auto& e : in.evidence) {
    Json::Value j;
    j["f"] = static_cast<Json::Int64>(e.from), j["t"] = static_cast<Json::Int64>(e.to), j["a"] = e.area;
    j["c"] = e.category, j["l"] = e.linkType + e.linkId, j["p"] = e.precedence;
    v["evidence"].append(j);
  }
  for (const auto& t : in.targets) {
    Json::Value j;
    j["id"] = t.id, j["a"] = t.area, j["m"] = t.metric, j["n"] = t.amount;
    j["ach"] = t.achieved ? Json::Value(*t.achieved) : Json::Value();
    j["pa"] = t.priorApplicable, j["pm"] = t.priorMet;
    v["targets"].append(j);
  }
  for (const auto& r : in.routines) {
    Json::Value j;
    j["id"] = r.id, j["a"] = r.applicable, j["c"] = r.completed;
    v["routines"].append(j);
  }
  for (const auto& [k, w] : in.settings.weights) v["w"][k] = w;
  for (const auto& [k, a] : in.settings.alpha) v["alpha"][k] = a;
  const auto digest = crypto::sha256(json::compact(v));
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const auto c : digest) {
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
  }
  return out;
}

DayResult scoreDay(const DayInputs& in) {
  DayResult r;
  r.inputsHash = hashInputs(in);
  const auto owner = resolveMinutes(in, in.dayTo);
  for (const auto o : owner) {
    if (o >= 0) ++r.areaMinutes[in.evidence[static_cast<std::size_t>(o)].area];
  }

  std::vector<PlanItem> items = in.items;
  for (auto& i : items) {
    if (i.type == "block" && !i.excluded) i.completion = blockCompletion(in, owner, i, in.dayTo);
  }

  // Targets.
  struct TargetOut {
    double achieved = 0, ratio = 0, over = 0;
  };
  std::vector<TargetOut> tout(in.targets.size());
  for (std::size_t k = 0; k < in.targets.size(); ++k) {
    const auto& t = in.targets[k];
    double ach = t.achieved.value_or(0);
    if (!t.achieved && t.metric == "minutes") {
      ach = minutesIn(in, owner, in.dayFrom, in.dayTo, [&](const Evidence& e) { return targetMatches(t, e); });
    }
    tout[k].achieved = ach;
    tout[k].ratio = t.amount > 0 ? std::min(ach / t.amount, 1.0) : 0;
    tout[k].over = t.amount > 0 ? std::max(0.0, ach / t.amount - 1.0) : 0;
  }

  // Areas.
  Json::Value areas(Json::objectValue);
  std::map<std::string, double> areaScore;
  for (const auto& a : scoredAreas()) {
    Json::Value out;
    double sw = 0, swc = 0;
    out["items"] = Json::Value(Json::arrayValue);
    for (const auto& i : items) {
      if (i.area != a) continue;
      Json::Value j;
      j["type"] = i.type, j["id"] = i.id, j["title"] = i.title, j["weight"] = round2(i.weight);
      j["completion"] = round2(i.completion);
      if (i.excluded) j["excluded"] = true;
      out["items"].append(j);
      if (i.excluded) continue;
      sw += i.weight;
      swc += i.weight * i.completion;
    }
    out["targets"] = Json::Value(Json::arrayValue);
    std::vector<double> ratios, overs;
    for (std::size_t k = 0; k < in.targets.size(); ++k) {
      if (in.targets[k].area != a) continue;
      Json::Value j;
      j["id"] = in.targets[k].id, j["title"] = in.targets[k].title, j["metric"] = in.targets[k].metric;
      j["amount"] = in.targets[k].amount, j["achieved"] = round2(tout[k].achieved), j["ratio"] = round2(tout[k].ratio);
      out["targets"].append(j);
      ratios.push_back(tout[k].ratio);
      overs.push_back(tout[k].over);
    }
    const bool hasPlan = sw > 0, hasTargets = !ratios.empty();
    out["applicable"] = hasPlan || hasTargets;
    if (!hasPlan && !hasTargets) {
      areas[a] = out;
      continue;
    }
    const double alpha = in.settings.alpha.count(a) ? in.settings.alpha.at(a) : 0.8;
    const double adherence = hasPlan ? swc / sw : 0;
    const double quantity = hasTargets ? std::accumulate(ratios.begin(), ratios.end(), 0.0) / static_cast<double>(ratios.size()) : 0;
    const double bonus = hasTargets ? std::min(5.0, 25.0 * std::accumulate(overs.begin(), overs.end(), 0.0) /
                                                            static_cast<double>(overs.size()))
                                    : 0;
    double score;
    if (hasPlan && hasTargets) score = std::min(100.0, 100.0 * (alpha * adherence + (1 - alpha) * quantity) + bonus);
    else if (hasPlan) score = 100.0 * adherence;
    else score = std::min(100.0, 100.0 * quantity + bonus);
    if (hasPlan) out["adherence"] = round2(adherence);
    if (hasTargets) out["quantity"] = round2(quantity);
    out["alpha"] = alpha;
    out["bonus"] = round2(bonus);
    out["score"] = round2(score);
    areaScore[a] = score;
    areas[a] = out;
  }

  // Consistency (§4.4).
  Json::Value cons;
  cons["items"] = Json::Value(Json::arrayValue);
  std::vector<double> rates;
  for (const auto& rt : in.routines) {
    if (rt.applicable <= 0) continue;
    const double rate = static_cast<double>(rt.completed) / rt.applicable;
    Json::Value j;
    j["type"] = "routine", j["id"] = rt.id, j["title"] = rt.title, j["completed"] = rt.completed;
    j["applicable"] = rt.applicable, j["rate"] = round2(rate);
    cons["items"].append(j);
    rates.push_back(rate);
  }
  for (std::size_t k = 0; k < in.targets.size(); ++k) {
    const auto& t = in.targets[k];
    if (!t.trackConsistency) continue;
    const int applicable = t.priorApplicable + 1;
    const int met = t.priorMet + (tout[k].ratio >= 1.0 ? 1 : 0);
    const double rate = static_cast<double>(met) / applicable;
    Json::Value j;
    j["type"] = "target", j["id"] = t.id, j["title"] = t.title, j["completed"] = met, j["applicable"] = applicable;
    j["rate"] = round2(rate);
    cons["items"].append(j);
    rates.push_back(rate);
  }
  if (!rates.empty()) {
    const double score = 100.0 * std::accumulate(rates.begin(), rates.end(), 0.0) / static_cast<double>(rates.size());
    cons["score"] = round2(score);
    areaScore["consistency"] = score;
  }
  r.consistency = cons;

  // Preemption attribution (§6.2): what entertainment displaced.
  const auto isEnt = [](const Evidence& e) { return e.area == "entertainment"; };
  const int entMinutes = minutesIn(in, owner, in.dayFrom, in.dayTo, isEnt);
  for (const auto& i : items) {
    if (i.excluded || i.completion >= 1.0) continue;
    if (i.type == "block") {
      const int m = minutesIn(in, owner, std::max(i.from, in.dayFrom), std::min(i.to, in.dayTo), isEnt);
      if (m > 0) r.preemptions.push_back({"overlap", i.type, i.id, i.title, m});
    } else if (i.due && entMinutes >= 15) {
      r.preemptions.push_back({"while_due", i.type, i.id, i.title, entMinutes});
    }
  }

  Json::Value weights(Json::objectValue);
  for (const auto& [k, w] : in.settings.weights) weights[k] = w;
  r.breakdown["areas"] = areas;
  r.breakdown["weights"] = weights;
  r.breakdown["profile"] = in.profile;
  Json::Value minutes(Json::objectValue);
  for (const auto& [a, m] : r.areaMinutes) minutes[a] = m;
  r.breakdown["evidence_minutes"] = minutes;

  if (!in.scored) {
    r.status = "not_scored";
    r.breakdown["reason"] = "the day's profile (" + in.profile + ") is not scored";
    return r;
  }
  if (!in.observed) {
    r.status = "unobserved";
    r.breakdown["reason"] = "no evidence of any kind for this day";
    return r;
  }
  double sw = 0, sws = 0;
  for (const auto& [a, s] : areaScore) {
    const double w = in.settings.weights.count(a) ? in.settings.weights.at(a) : 0;
    sw += w;
    sws += w * s;
  }
  if (sw <= 0) {
    r.status = "not_scored";
    r.breakdown["reason"] = "nothing was planned and no target applied";
    return r;
  }
  r.status = "final";
  r.base = round2(sws / sw);
  return r;
}

Json::Value progress(const DayInputs& in, int64_t now) {
  const auto owner = resolveMinutes(in, now);
  Json::Value areas(Json::objectValue);
  for (const auto& a : scoredAreas()) {
    Json::Value out;
    int planned = 0, done = 0;
    double blockPlanned = 0, blockCovered = 0;
    for (const auto& i : in.items) {
      if (i.area != a || i.excluded) continue;
      if (i.type == "block") {
        if (i.from >= now) continue;
        const double mins = static_cast<double>(std::min({i.to, in.dayTo, now}) - std::max(i.from, in.dayFrom)) / 60.0;
        if (mins <= 0) continue;
        blockPlanned += mins;
        blockCovered += blockCompletion(in, owner, i, now) * mins;
      } else {
        ++planned;
        if (i.completion >= 1.0) ++done;
      }
    }
    Json::Value targets(Json::arrayValue);
    for (const auto& t : in.targets) {
      if (t.area != a) continue;
      double ach = t.achieved.value_or(0);
      if (!t.achieved && t.metric == "minutes") {
        ach = minutesIn(in, owner, in.dayFrom, now, [&](const Evidence& e) { return targetMatches(t, e); });
      }
      Json::Value j;
      j["title"] = t.title, j["metric"] = t.metric, j["amount"] = t.amount, j["achieved"] = round2(ach);
      targets.append(j);
    }
    if (planned == 0 && blockPlanned == 0 && targets.empty()) continue;
    out["planned_items"] = planned;
    out["done_items"] = done;
    out["block_minutes_so_far"] = static_cast<int>(blockPlanned);
    out["block_minutes_covered"] = static_cast<int>(blockCovered);
    out["targets"] = targets;
    areas[a] = out;
  }
  Json::Value p;
  p["local_day"] = in.localDay;
  p["profile"] = in.profile;
  p["areas"] = areas;
  return p;
}

}  // namespace thelife::scoring
