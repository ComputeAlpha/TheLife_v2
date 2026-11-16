// Stage 4 exit gate, the slow half (prelim_spec.md §Stage 4): real focus on a synthetic window under Xvfb becomes
// a process detection through the backend's own rules, and the capture of the same minutes a vision detection.
// Runs about three minutes, because a block must last 2 min to become a detection (backend_lld.md §7.4); the
// ctest label "slow" lets a quick run leave it out (ctest -LE slow).
#include <thread>

#include "e2e/fixture.h"

TEST_F(EndToEnd, TwoMinutesInAnEditorBecomeAProcessDetection) {
  fake_->script(J(R"([{"content":"","tool_calls":[{"name":"submit_vision_analysis","arguments":{
      "area":"work","category":"coding","summary":"Editing main.cc of TheLife in VS Code","confidence":0.8,
      "apps":["VS Code"]}}]}])"));
  ASSERT_EQ(cli({"enroll", "--url", server_->baseUrl()}, token_ + "\n"), 0) << readFile(log_);
  XClient x(xvfb_->display());
  const auto editor = x.createWindow(100, 100, 1200, 800, 0x1e1e1e, "code", "main.cc — TheLife_v2 — Visual Studio Code",
                                     static_cast<uint32_t>(::getpid()));
  const auto terminal = x.createWindow(2000, 100, 800, 600, 0x000000, "gnome-terminal-server", "bash", std::nullopt);
  x.activate(editor);
  x.fakeMotion(10, 10);
  collector_ = std::make_unique<Child>(std::vector<std::string>{THELIFE_COLLECTOR_BIN, "run"}, env(), log_);

  // 150 s of someone working: input every few seconds, so nothing counts as idle.
  const auto until = std::chrono::steady_clock::now() + 150s;
  for (int i = 0; std::chrono::steady_clock::now() < until; ++i) {
    x.fakeMotion(10 + i % 50, 10);
    std::this_thread::sleep_for(5s);
  }
  x.activate(terminal);  // the editor's focus span ends here
  x.fakeMotion(2100, 200);

  Json::Value item;
  ASSERT_TRUE(waitUntil(
      [&] {
        const auto items = api_->get("/detections?source=process").json["items"];  // a copy: not a temporary's member
        for (const auto& d : items) {
          if (d["summary"].asString() == "VS Code: TheLife_v2") item = d;
        }
        return !item.isNull();
      },
      60s))
      << api_->get("/detections").body;
  EXPECT_EQ(item["area"].asString(), "work");
  EXPECT_EQ(item["category"].asString(), "coding");
  EXPECT_EQ(item["status"].asString(), "detected");
  EXPECT_EQ(item["device_id"].asString(), deviceId_);
  const auto start = parseTime(item["started_at"].asString()), end = parseTime(item["ended_at"].asString());
  EXPECT_GE(end - start, 140) << item.toStyledString();  // the whole focus, not a fragment
  EXPECT_LE(end - start, 170) << item.toStyledString();
  EXPECT_EQ(item["evidence"]["app"].asString(), "VS Code");

  // The same minutes seen by the vision model, from the first capture.
  ASSERT_TRUE(waitUntil([&] { return api_->get("/detections?source=vision").json["items"].size() >= 1; }, 30s));

  // The user's correction goes through the same API the timeline uses.
  const auto claim = api_->post("/detections/" + item["id"].asString() + ":claim",
                                J(R"({"area": "skill", "category": "course", "summary": "C++ course exercise"})"));
  ASSERT_EQ(claim.status, 201) << claim.body;
  EXPECT_EQ(api_->get("/detections/" + item["id"].asString()).json["status"].asString(), "superseded");
  EXPECT_EQ(collector_->stop(SIGTERM), 0);
}
