// Collector ingest derivation (backend_lld.md §7.4): window classification, title parsers, focus spans with idle
// and silence cuts, and blocks that become process detections. No database.
#include <gtest/gtest.h>

#include "core/json.h"
#include "ingest/derive.h"
#include "ingest/rules.h"

using namespace thelife;
using namespace thelife::ingest;

namespace {

Window win(const std::string& inst, const std::string& cls, const std::string& title, const std::string& exe = "") {
  return {inst, cls, title, exe};
}

Obs window(int64_t at, const std::string& inst, const std::string& cls, const std::string& title) {
  Json::Value p;
  p["window_id"] = "0x1";
  p["wm_class"].append(inst);
  p["wm_class"].append(cls);
  p["title"] = title;
  return {at, 0, "window", p};
}

Obs idle(int64_t at, const std::string& event, int64_t since = 0, bool passive = false, bool locked = false) {
  Json::Value p;
  p["event"] = event;
  p["at"] = "x";
  p["passive"] = passive;
  p["locked"] = locked;
  return {at, since, "idle", p};
}

Obs tick(int64_t at) { return {at, 0, "process_snapshot", json::parse(R"({"full":false})")}; }

}  // namespace

TEST(IngestRules, DefaultsClassifyCommonAppsAndBrowserPages) {
  auto c = classify({}, win("code", "Code", "\xe2\x97\x8f main.cc \xe2\x80\x94 TheLife_v2 \xe2\x80\x94 Visual Studio Code"));
  EXPECT_EQ(c.app, "VS Code");
  EXPECT_EQ(c.area, "work");
  EXPECT_EQ(c.subject, "TheLife_v2");

  c = classify({}, win("Navigator", "firefox", "Lo-fi beats to study to - YouTube \xe2\x80\x94 Mozilla Firefox"));
  EXPECT_EQ(c.app, "YouTube");  // the title rule is more specific than the plain browser rule
  EXPECT_EQ(c.area, "entertainment");
  EXPECT_EQ(c.subject, "Lo-fi beats to study to - YouTube");

  c = classify({}, win("Navigator", "firefox", "Weather forecast \xe2\x80\x94 Mozilla Firefox"));
  EXPECT_EQ(c.app, "Firefox");
  EXPECT_LT(c.confidence, 0.4);  // general browsing does not count until vision says more

  c = classify({}, win("jetbrains-clion", "jetbrains-clion", "TheLife_v2 \xe2\x80\x93 runner.cc"));
  EXPECT_EQ(c.app, "JetBrains IDE");
  EXPECT_EQ(c.subject, "TheLife_v2");

  c = classify({}, win("someapp", "SomeApp", "Window"));
  EXPECT_FALSE(c.matched);
  EXPECT_EQ(c.category, "unknown");
  EXPECT_EQ(c.app, "SomeApp");
}

TEST(IngestRules, UserRulesComeFirstAndMatchCaseInsensitively) {
  Rule figma;
  figma.matchWmClass = "FIREFOX";
  figma.matchTitle = "figma";
  figma.app = "Figma";
  figma.category = "design";
  figma.area = "work";
  figma.confidence = 0.8;
  Rule exe;
  exe.matchExe = "blender";
  exe.app = "Blender";
  exe.category = "3d";
  exe.area = "skill";
  const auto c = classify({exe, figma}, win("Navigator", "firefox", "Landing page \xe2\x80\x93 Figma \xe2\x80\x94 Mozilla Firefox"));
  EXPECT_EQ(c.app, "Figma");
  EXPECT_DOUBLE_EQ(c.confidence, 0.8);
  EXPECT_FALSE(c.subject.empty());  // the browser parser still applies
  EXPECT_EQ(classify({exe, figma}, win("x", "y", "t", "/opt/blender-4.2/blender")).app, "Blender");
}

TEST(IngestRules, ConfigFileParsesAndBadRulesAreRejected) {
  EXPECT_GT(defaultRules().size(), 20u);
  EXPECT_THROW(parseRules(json::parse(R"({"rules":[{"app":"x","category":"y","area":"work"}]})")),
               std::invalid_argument);  // no match field
  EXPECT_THROW(parseRules(json::parse(R"({"rules":[{"match_exe":"x","app":"x","category":"y","area":"work",
                                         "parser":"nope"}]})")),
               std::invalid_argument);
}

TEST(FocusSpans, WindowChangesIdleAndSilenceCutSpans) {
  const std::vector<Obs> obs{
      window(100, "code", "Code", "a.cc \xe2\x80\x94 P \xe2\x80\x94 Visual Studio Code"),
      tick(160), tick(220),
      window(400, "Navigator", "firefox", "Docs \xe2\x80\x94 Mozilla Firefox"),
      idle(700, "start", 580),  // reported at 700, idle since 580
      idle(900, "end"),
      tick(960),
      tick(2000),  // 1040 s of silence: the span ends at the last observation before it
      window(2100, "code", "Code", "b.cc \xe2\x80\x94 P \xe2\x80\x94 Visual Studio Code"),
  };
  const auto r = focusSpans(std::nullopt, false, 50, obs);
  ASSERT_EQ(r.spans.size(), 4u);
  EXPECT_EQ(r.spans[0].from, 100);
  EXPECT_EQ(r.spans[0].to, 400);
  EXPECT_EQ(r.spans[0].window.wmClass, "Code");
  EXPECT_EQ(r.spans[1].from, 400);
  EXPECT_EQ(r.spans[1].to, 580);  // backdated idle start
  EXPECT_EQ(r.spans[2].from, 900);  // after idle, the same window again
  EXPECT_EQ(r.spans[2].to, 960);    // silence: ends at the last observation before it
  EXPECT_EQ(r.spans[2].window.wmClass, "firefox");
  EXPECT_EQ(r.spans[3].from, 2000);
  EXPECT_EQ(r.spans[3].to, 2100);
  EXPECT_EQ(r.until, 2100);  // the VS Code span that began at 2100 is still open
}

TEST(FocusSpans, OpenSpanStaysOpenUntilLongAndPassiveIdleDoesNotCut) {
  std::vector<Obs> obs{window(0, "mpv", "mpv", "film.mkv - mpv"), idle(300, "start", 180, /*passive=*/true)};
  for (int t = 360; t <= 1200; t += 60) obs.push_back(tick(t));
  auto r = focusSpans(std::nullopt, false, -1, obs);
  EXPECT_TRUE(r.spans.empty());
  EXPECT_EQ(r.until, 0);  // re-read from the window's start next time

  for (int t = 1260; t <= 2000; t += 60) obs.push_back(tick(t));
  r = focusSpans(std::nullopt, false, -1, obs);
  ASSERT_EQ(r.spans.size(), 1u);  // 30 min reached: closed at the last observation and continued
  EXPECT_EQ(r.spans[0].to, 1980);
  EXPECT_EQ(r.until, 1980);

  // Locking cuts even a fullscreen window.
  r = focusSpans(std::nullopt, false, -1, {window(0, "mpv", "mpv", "x"), tick(300), idle(600, "start", 0, true, true)});
  ASSERT_EQ(r.spans.size(), 1u);
  EXPECT_EQ(r.spans[0].to, 600);
}

TEST(FocusSpans, ResumesFromTheStateAtTheCursor) {
  const auto r = focusSpans(win("code", "Code", "x \xe2\x80\x94 P \xe2\x80\x94 Visual Studio Code"), false, 1000,
                            {tick(1060), window(1200, "Navigator", "firefox", "y")});
  ASSERT_EQ(r.spans.size(), 1u);
  EXPECT_EQ(r.spans[0].from, 1000);
  EXPECT_EQ(r.spans[0].to, 1200);
  EXPECT_EQ(r.until, 1200);
  EXPECT_TRUE(focusSpans(std::nullopt, true, 1000, {tick(1060)}).spans.empty());  // idle at the cursor
}

TEST(Blocks, MergeAcrossShortGapsDropGlancesAndKeepSubjects) {
  const auto vs = [](const std::string& f, const std::string& p) {
    return win("code", "Code", f + " \xe2\x80\x94 " + p + " \xe2\x80\x94 Visual Studio Code");
  };
  const std::vector<Span> spans{
      {0, 600, vs("a.cc", "TheLife")},
      {600, 610, win("Navigator", "firefox", "glance \xe2\x80\x94 Mozilla Firefox")},  // 10 s: ignored
      {610, 900, vs("b.cc", "TheLife")},
      {900, 1000, vs("c.py", "Scripts")},
      {1100, 1150, win("Navigator", "firefox", "YouTube \xe2\x80\x94 Mozilla Firefox")},  // < 120 s block: dropped
  };
  const auto bs = blocks(spans, [](const Window& w) { return classify({}, w); });
  ASSERT_EQ(bs.size(), 1u);
  EXPECT_EQ(bs[0].from, 0);
  EXPECT_EQ(bs[0].to, 1000);
  EXPECT_EQ(bs[0].spans, 3);
  EXPECT_EQ(summaryOf(bs[0]), "VS Code: TheLife (+1 more)");
  const auto e = evidenceOf(bs[0]);
  EXPECT_EQ(e["subjects"][0]["subject"].asString(), "TheLife");
  EXPECT_EQ(e["subjects"][0]["minutes"].asInt(), 15);
}
