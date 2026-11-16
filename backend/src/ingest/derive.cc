#include "ingest/derive.h"

#include <algorithm>

namespace thelife::ingest {

Window windowOf(const Json::Value& p) {
  Window w;
  if (p["wm_class"].isArray() && p["wm_class"].size() == 2) {
    w.wmInstance = p["wm_class"][0].asString();
    w.wmClass = p["wm_class"][1].asString();
  }
  w.title = p.get("title", "").asString();
  w.exe = p["exe"].isString() ? p["exe"].asString() : "";
  return w;
}

SpanResult focusSpans(std::optional<Window> current, bool idle, int64_t from, const std::vector<Obs>& obs,
                      const SpanParams& p) {
  SpanResult r;
  int64_t start = from, lastSeen = from;
  auto close = [&](int64_t at) {
    if (current && !idle && at > start) r.spans.push_back({start, at, *current});
    start = at;
  };
  for (const auto& o : obs) {
    if (o.at < lastSeen) continue;  // out of order: ignore rather than produce negative spans
    if (o.at - lastSeen > p.silenceSec) {
      close(lastSeen);
      start = o.at;
    }
    if (o.kind == "window") {
      close(o.at);
      current = windowOf(o.payload);
    } else if (o.kind == "idle") {
      const auto event = o.payload.get("event", "").asString();
      if (event == "start" && !o.payload.get("passive", false).asBool()) {
        // Backdated to the last input, but never before the span began or after it was reported.
        close(std::clamp(o.eventAt ? o.eventAt : o.at, start, o.at));
        idle = true;
      } else if (event == "start" && o.payload.get("locked", false).asBool()) {
        close(o.at);
        idle = true;
      } else if (event == "end") {
        if (idle) start = o.at;
        idle = false;
      }
    } else if (o.kind == "session") {
      close(o.at);
      current.reset();  // a new X session reports its window again
    } else if (o.kind == "gap") {
      close(o.at);
    }
    lastSeen = o.at;
  }
  if (current && !idle) {
    if (lastSeen - start >= p.openMaxSec) close(lastSeen);
    r.until = start;  // the open span is re-read next time
  } else {
    r.until = lastSeen;
  }
  return r;
}

std::vector<Block> blocks(const std::vector<Span>& spans, const std::function<Classification(const Window&)>& classify,
                          const BlockParams& p) {
  std::vector<Block> out;
  for (const auto& s : spans) {
    const int64_t len = s.to - s.from;
    if (len < p.minSpanSec) continue;
    const auto c = classify(s.window);
    if (!out.empty()) {
      auto& b = out.back();
      if (b.c.app == c.app && b.c.category == c.category && b.c.area == c.area && s.from - b.to <= p.mergeGapSec) {
        b.to = s.to;
        if (!c.subject.empty()) b.subjects[c.subject] += len;
        b.c.confidence = std::max(b.c.confidence, c.confidence);
        ++b.spans;
        continue;
      }
    }
    Block b;
    b.from = s.from;
    b.to = s.to;
    b.c = c;
    if (!c.subject.empty()) b.subjects[c.subject] = len;
    b.wmClass = s.window.wmClass;
    b.exe = s.window.exe;
    b.spans = 1;
    out.push_back(std::move(b));
  }
  out.erase(std::remove_if(out.begin(), out.end(), [&](const Block& b) { return b.to - b.from < p.minBlockSec; }),
            out.end());
  return out;
}

namespace {
std::vector<std::pair<std::string, int64_t>> topSubjects(const Block& b, std::size_t n) {
  std::vector<std::pair<std::string, int64_t>> v(b.subjects.begin(), b.subjects.end());
  std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
  if (v.size() > n) v.resize(n);
  return v;
}
}  // namespace

std::string summaryOf(const Block& b) {
  const auto top = topSubjects(b, 1);
  std::string s = b.c.app;
  if (!top.empty() && !top[0].first.empty() && top[0].first != b.c.app) s += ": " + top[0].first;
  if (b.subjects.size() > 1) s += " (+" + std::to_string(b.subjects.size() - 1) + " more)";
  if (s.size() > 500) s.resize(497), s += "...";
  return s;
}

Json::Value evidenceOf(const Block& b) {
  Json::Value e;
  e["app"] = b.c.app;
  e["rule_matched"] = b.c.matched;
  e["wm_class"] = b.wmClass;
  if (!b.exe.empty()) e["exe"] = b.exe;
  e["spans"] = b.spans;
  e["subjects"] = Json::Value(Json::arrayValue);
  for (const auto& [subject, sec] : topSubjects(b, 5)) {
    Json::Value s;
    s["subject"] = subject;
    s["minutes"] = static_cast<Json::Int64>((sec + 30) / 60);
    e["subjects"].append(s);
  }
  return e;
}

}  // namespace thelife::ingest
