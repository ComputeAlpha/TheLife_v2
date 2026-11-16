#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/json.h"
#include "probes/idle_probe.h"
#include "probes/lock_probe.h"
#include "probes/proc_probe.h"
#include "probes/window_probe.h"
#include "support/fakes.h"
#include "support/schema.h"

using namespace thelife::collector;
using namespace thelife::collector::testing;
namespace fs = std::filesystem;

namespace {

SysTime at(double s) { return SysTime(std::chrono::milliseconds(static_cast<int64_t>(s * 1000))); }

struct Emitted {
  SysTime at;
  Json::Value payload;
};

WindowInfo win(uint32_t id, std::string cls, std::string title, std::optional<int> pid = std::nullopt) {
  WindowInfo w;
  w.id = id;
  w.wmInstance = cls;
  w.wmClass = cls;
  w.title = std::move(title);
  w.pid = pid;
  w.geometry = Rect{2000, 100, 800, 600};
  return w;
}

}  // namespace

TEST(WindowProbe, ReportsASwitchAtOnceAndDebouncesTitleChanges) {
  FakeX x;
  x.mons = {{0, {0, 0, 1920, 1080}, true}, {1, {1920, 0, 1920, 1080}, false}};
  x.windows[0x3a0000f] = win(0x3a0000f, "Code", "main.cc — TheLife_v2 — Visual Studio Code");
  x.windows[0x400002] = win(0x400002, "firefox", "Inbox");
  x.active = 0x3a0000f;
  std::vector<Emitted> out;
  WindowProbe p(x, "/nonexistent", [&](SysTime t, const Json::Value& j) { out.push_back({t, j}); });
  p.setMonitors(x.mons);
  EXPECT_EQ(p.refresh(at(100), true), 0x3a0000fu);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload["window_id"].asString(), "0x3a0000f");
  EXPECT_EQ(out[0].payload["monitor"].asInt(), 1);
  EXPECT_EQ(out[0].payload["wm_class"][1].asString(), "Code");
  expectValid("window", out[0].payload);

  // An animated title: three changes within two seconds produce one row, with the last value.
  for (int i = 0; i < 3; ++i) {
    x.windows[0x3a0000f].title = "build " + std::to_string(i);
    p.refresh(at(101 + i * 0.5), false);
    p.tick(at(101 + i * 0.5));
  }
  EXPECT_EQ(out.size(), 1u);
  p.tick(at(104.1));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1].payload["title"].asString(), "build 2");
  EXPECT_EQ(out[1].at, at(102));

  // A title change, then a switch before the quiet period ends: only the switch is reported.
  x.windows[0x3a0000f].title = "half-typed";
  p.refresh(at(110), false);
  x.active = 0x400002;
  p.refresh(at(110.5), true);
  p.tick(at(120));
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2].payload["title"].asString(), "Inbox");
  EXPECT_EQ(out[2].at, at(110.5));
}

TEST(WindowProbe, AClockInTheTitleIsStillReportedEveryTenSeconds) {
  FakeX x;
  x.windows[1] = win(1, "term", "t0");
  x.active = 1;
  std::vector<Emitted> out;
  WindowProbe p(x, "/nonexistent", [&](SysTime t, const Json::Value& j) { out.push_back({t, j}); });
  p.refresh(at(0), true);
  for (int s = 1; s <= 12; ++s) {
    x.windows[1].title = "t" + std::to_string(s);
    p.refresh(at(s), false);
    p.tick(at(s));
  }
  EXPECT_EQ(out.size(), 2u);  // the initial switch and one forced flush after 10 s
}

TEST(WindowProbe, NoFocusedWindowIsAnEmptyButValidRow) {
  FakeX x;
  std::vector<Emitted> out;
  WindowProbe p(x, "/nonexistent", [&](SysTime t, const Json::Value& j) { out.push_back({t, j}); });
  p.refresh(at(0), true);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload["window_id"].asString(), "0x0");
  expectValid("window", out[0].payload);
  // Long and odd titles are cut to the schema's limits.
  x.windows[2] = win(2, std::string(400, 'c'), std::string(3000, 'x') + "\x01");
  x.active = 2;
  p.refresh(at(1), true);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1].payload["title"].asString().size(), 1024u);
  expectValid("window", out[1].payload);
}

TEST(IdleProbe, StartIsBackdatedAndEndMarksTheFirstInput) {
  std::vector<Emitted> out;
  IdleProbe p([&](SysTime t, const Json::Value& j) { out.push_back({t, j}); });
  p.sample(at(1000), 60'000, false, false, 120);
  EXPECT_TRUE(out.empty());
  p.sample(at(1065), 125'000, false, false, 120);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload["event"].asString(), "start");
  EXPECT_EQ(out[0].payload["at"].asString(), rfc3339(at(940)));  // the last input
  EXPECT_FALSE(out[0].payload["passive"].asBool());
  expectValid("idle", out[0].payload);
  EXPECT_TRUE(p.idle());
  p.sample(at(1070), 130'000, false, false, 120);
  EXPECT_EQ(out.size(), 1u);
  p.sample(at(1200), 3'000, false, false, 120);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1].payload["event"].asString(), "end");
  EXPECT_EQ(out[1].payload["at"].asString(), rfc3339(at(1197)));
  expectValid("idle", out[1].payload);
  EXPECT_FALSE(p.idle());
}

TEST(IdleProbe, LockIsIdleAtOnceAndFullscreenIsPassive) {
  std::vector<Emitted> out;
  IdleProbe p([&](SysTime t, const Json::Value& j) { out.push_back({t, j}); });
  p.sample(at(10), 2'000, true, false, 120);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].payload["locked"].asBool());
  EXPECT_EQ(out[0].payload["at"].asString(), rfc3339(at(10)));
  EXPECT_TRUE(p.locked());
  p.sample(at(20), 1'000, true, false, 120);  // typing the password: still locked
  EXPECT_EQ(out.size(), 1u);
  p.sample(at(30), 500, false, false, 120);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[1].payload["event"].asString(), "end");

  p.sample(at(500), 200'000, false, true, 120);  // a video, nobody touching anything
  ASSERT_EQ(out.size(), 3u);
  EXPECT_TRUE(out[2].payload["passive"].asBool());
  EXPECT_TRUE(p.passive());
  p.sample(at(505), 205'000, false, false, 120);  // the video ended
  ASSERT_EQ(out.size(), 4u);
  EXPECT_FALSE(out[3].payload["passive"].asBool());
  EXPECT_FALSE(p.passive());
}

TEST(LockProbe, KnowsCommonLockers) {
  EXPECT_TRUE(LockProbe::isLockerClass("i3lock"));
  EXPECT_TRUE(LockProbe::isLockerClass("XSecureLock"));
  EXPECT_FALSE(LockProbe::isLockerClass("Code"));
}

namespace {

// A fake /proc: one directory per pid with status, stat, cmdline and an exe link.
struct FakeProc {
  TempDir dir;
  void add(int pid, int uid, int ppid, const std::string& exe, const std::string& comm,
           const std::vector<std::string>& argv, unsigned long ticks, long rssKb) {
    const auto d = dir.path / std::to_string(pid);
    fs::create_directories(d);
    std::ofstream(d / "status") << "Name:\t" << comm << "\nPPid:\t" << ppid << "\nUid:\t" << uid << "\t" << uid
                                << "\t" << uid << "\t" << uid << "\nVmRSS:\t" << rssKb << " kB\n";
    std::ofstream(d / "stat") << pid << " (" << comm << ") S " << ppid << " 1 1 0 -1 0 0 0 0 0 " << ticks / 2 << " "
                              << ticks - ticks / 2 << " 0 0 20 0 1 0 100 0 0\n";
    std::string cmd;
    for (const auto& a : argv) cmd += a + std::string(1, '\0');
    std::ofstream(d / "cmdline", std::ios::binary) << cmd;
    std::error_code ec;
    fs::remove(d / "exe", ec);
    if (!exe.empty()) fs::create_symlink(exe, d / "exe");
  }
  void remove(int pid) { fs::remove_all(dir.path / std::to_string(pid)); }
};

}  // namespace

TEST(ProcProbe, FullSnapshotKeepsOnlyTheUsersProcessesGroupedByExecutable) {
  FakeProc fp;
  const int me = static_cast<int>(::getuid());
  fp.add(100, me, 1, "/usr/share/code/code", "code", {"/usr/share/code/code", "--folder-uri", "file:///x"}, 1000,
         204800);
  fp.add(101, me, 100, "/usr/share/code/code", "code", {"/usr/share/code/code", "--type=renderer"}, 500, 102400);
  fp.add(200, me, 1, "/usr/bin/bash", "bash", {"bash", "--secret=hunter2"}, 10, 4096);
  fp.add(300, me + 1, 1, "/usr/bin/other", "other", {"other"}, 10, 1024);  // another user
  fp.add(400, 0, 2, "", "kworker/0:1", {}, 10, 0);                          // kernel thread
  ProcProbe p(fp.dir.str(), static_cast<uid_t>(me), {"code"});
  const auto s = p.snapshot(at(0), 101, {100});
  expectValid("process_snapshot", s);
  ASSERT_TRUE(s["full"].asBool());
  ASSERT_EQ(s["procs"].size(), 2u);
  std::map<std::string, Json::Value> by;
  for (const auto& e : s["procs"]) by[e["exe"].asString()] = e;
  ASSERT_TRUE(by.count("/usr/share/code/code"));
  const auto& code = by["/usr/share/code/code"];
  EXPECT_EQ(code["pids"].size(), 2u);
  EXPECT_DOUBLE_EQ(code["rss_mb"].asDouble(), 300.0);
  EXPECT_TRUE(code["has_window"].asBool());
  EXPECT_TRUE(code["focused"].asBool());
  EXPECT_EQ(code["args"][0].asString(), "--folder-uri");  // allowlisted
  ASSERT_TRUE(by.count("/usr/bin/bash"));
  EXPECT_FALSE(by["/usr/bin/bash"].isMember("args"));  // not allowlisted: arguments may carry secrets
  EXPECT_EQ(json::compact(s).find("hunter2"), std::string::npos);
}

TEST(ProcProbe, DeltasReportStartedExitedAndChangedThenAFullOneAfterTenMinutes) {
  FakeProc fp;
  const int me = static_cast<int>(::getuid());
  const long hz = ::sysconf(_SC_CLK_TCK);
  fp.add(10, me, 1, "/usr/bin/a", "a", {"a"}, 0, 1024);
  fp.add(20, me, 1, "/usr/bin/b", "b", {"b"}, 0, 1024);
  ProcProbe p(fp.dir.str(), static_cast<uid_t>(me), {});
  p.snapshot(at(0), std::nullopt, {});
  // 60 s later: a used half a CPU, b exited, c started.
  fp.add(10, me, 1, "/usr/bin/a", "a", {"a"}, static_cast<unsigned long>(30 * hz), 1024);
  fp.remove(20);
  fp.add(30, me, 1, "/usr/bin/c", "c", {"c"}, 0, 1024);
  auto d = p.snapshot(at(60), std::nullopt, {});
  expectValid("process_snapshot", d);
  EXPECT_FALSE(d["full"].asBool());
  ASSERT_EQ(d["started"].size(), 1u);
  EXPECT_EQ(d["started"][0]["exe"].asString(), "/usr/bin/c");
  ASSERT_EQ(d["exited"].size(), 1u);
  EXPECT_EQ(d["exited"][0].asString(), "/usr/bin/b");
  ASSERT_EQ(d["changed"].size(), 1u);
  EXPECT_NEAR(d["changed"][0]["cpu_pct"].asDouble(), 50.0, 0.2);
  EXPECT_EQ(p.top(1)[0].exe, "/usr/bin/a");
  // Nothing changed: an empty delta, still sent as a keepalive.
  fp.add(10, me, 1, "/usr/bin/a", "a", {"a"}, static_cast<unsigned long>(60 * hz), 1024);
  d = p.snapshot(at(120), std::nullopt, {});
  expectValid("process_snapshot", d);
  EXPECT_EQ(json::compact(d), R"({"full":false})");
  d = p.snapshot(at(601), std::nullopt, {});
  EXPECT_TRUE(d["full"].asBool());
}

TEST(ProcProbe, CommandLinesFallBackWhenExeIsUnreadable) {
  FakeProc fp;
  const int me = static_cast<int>(::getuid());
  fp.add(5, me, 1, "", "python3", {"/usr/bin/python3", "script.py"}, 0, 1024);
  ProcProbe p(fp.dir.str(), static_cast<uid_t>(me), {});
  const auto s = p.snapshot(at(0), std::nullopt, {});
  ASSERT_EQ(s["procs"].size(), 1u);
  EXPECT_EQ(s["procs"][0]["exe"].asString(), "/usr/bin/python3");
}

TEST(Schemas, TheCheckRejectsWhatTheBackendWould) {
  std::string why;
  Json::Value w;
  w["window_id"] = "3a0000f";  // no 0x
  w["wm_class"].append("a");
  w["title"] = "t";
  EXPECT_FALSE(validates("window", w, &why));
  Json::Value p;
  p["full"] = true;
  p["procs"].append(Json::Value(Json::objectValue));  // exe missing
  EXPECT_FALSE(validates("process_snapshot", p, &why));
  Json::Value c;
  c["skipped"] = "bored";
  EXPECT_FALSE(validates("capture_meta", c, &why));
}

TEST(Schemas, CaptureMetaGapAndSessionPayloadsAreValid) {
  Json::Value skipped;
  skipped["skipped"] = "offline";
  expectValid("capture_meta", skipped);
  Json::Value done;
  done["capture_id"] = "0196f1c2-7a3b-7c4d-8e5f-0123456789ab";
  done["monitors"] = 2;
  done["active_monitor"] = 1;
  done["duration_ms"] = 830;
  expectValid("capture_meta", done);
  Json::Value gap;
  gap["from_seq"] = 1;
  gap["to_seq"] = 9;
  gap["reason"] = "queue_full";
  expectValid("gap", gap);
  Json::Value session;
  session["event"] = "start";
  session["collector_version"] = "0.0.1";
  session["x_screen"] = ":0";
  Json::Value m;
  m["index"] = 0;
  m["x"] = 0;
  m["y"] = 0;
  m["width"] = 1920;
  m["height"] = 1080;
  m["primary"] = true;
  session["monitors"].append(m);
  expectValid("session", session);
}
