#include <gtest/gtest.h>
#include <turbojpeg.h>

#include "capture/image.h"
#include "sched/scheduler.h"

using namespace thelife::collector;

namespace {

Image solid(int w, int h, uint8_t b, uint8_t g, uint8_t r) {
  Image img;
  img.width = w;
  img.height = h;
  img.stride = w * 4;
  img.px = SecureBytes(static_cast<size_t>(img.stride) * h);
  for (int i = 0; i < w * h; ++i) {
    img.px.data()[i * 4 + 0] = b;
    img.px.data()[i * 4 + 1] = g;
    img.px.data()[i * 4 + 2] = r;
    img.px.data()[i * 4 + 3] = 0;
  }
  return img;
}

SysTime at(int64_t s) { return SysTime(std::chrono::seconds(s)); }

}  // namespace

TEST(Image, DownscaleKeepsAspectAndAveragesColour) {
  auto img = solid(3840, 1080, 10, 20, 30);
  // Left half black, right half white: the scaled midpoint column averages, the edges stay pure.
  for (int y = 0; y < 1080; ++y) {
    for (int x = 1920; x < 3840; ++x) std::fill_n(img.px.data() + (y * 3840 + x) * 4, 3, uint8_t{255});
    for (int x = 0; x < 1920; ++x) std::fill_n(img.px.data() + (y * 3840 + x) * 4, 3, uint8_t{0});
  }
  const auto out = downscale(std::move(img), 1600);
  EXPECT_EQ(out.width, 1600);
  EXPECT_EQ(out.height, 450);
  EXPECT_EQ(out.px.data()[0], 0);
  EXPECT_EQ(out.px.data()[(1599) * 4], 255);
  EXPECT_TRUE(img.px.empty());  // the full-size pixels were wiped
}

TEST(Image, SmallImagesAreNotScaled) {
  const auto out = downscale(solid(800, 600, 1, 2, 3), 1600);
  EXPECT_EQ(out.width, 800);
  EXPECT_EQ(out.height, 600);
}

TEST(Image, EncodesAJpegThatDecodesToTheSameSize) {
  const auto img = solid(640, 360, 0, 0, 255);
  std::string err;
  const auto jpeg = encodeJpeg(img, 80, &err);
  ASSERT_TRUE(jpeg) << err;
  ASSERT_GT(jpeg->size(), 3u);
  EXPECT_LT(jpeg->size(), jpeg->capacity());
  EXPECT_EQ(jpeg->data()[0], 0xFF);
  EXPECT_EQ(jpeg->data()[1], 0xD8);
  EXPECT_EQ(jpeg->data()[2], 0xFF);
  tjhandle h = tjInitDecompress();
  int w = 0, hgt = 0, sub = 0, cs = 0;
  ASSERT_EQ(tjDecompressHeader3(h, jpeg->data(), jpeg->size(), &w, &hgt, &sub, &cs), 0);
  tjDestroy(h);
  EXPECT_EQ(w, 640);
  EXPECT_EQ(hgt, 360);
}

TEST(SecureBytes, WipeOnMoveAndRelease) {
  SecureBytes a(16);
  a.data()[0] = 7;
  SecureBytes b = std::move(a);
  EXPECT_EQ(a.size(), 0u);
  EXPECT_EQ(b.data()[0], 7);
  b.setSize(4);
  EXPECT_EQ(b.size(), 4u);
  EXPECT_EQ(b.capacity(), 16u);
  b.wipe();
  EXPECT_TRUE(b.empty());
}

namespace {
SchedState active() {
  SchedState s;
  s.configKnown = s.enabled = s.online = true;
  s.intervalS = 300;
  s.jitterPct = 10;
  return s;
}
}  // namespace

TEST(Scheduler, FirstCaptureAfterTheInitialDelayThenEveryIntervalWithJitter) {
  Scheduler sch(1, Seconds(60));
  EXPECT_EQ(sch.tick(at(0), active()).kind, SchedDecision::Kind::Wait);
  EXPECT_EQ(sch.tick(at(59), active()).kind, SchedDecision::Kind::Wait);
  EXPECT_EQ(sch.tick(at(60), active()).kind, SchedDecision::Kind::Capture);
  const auto next = *sch.nextDue() - at(60);
  EXPECT_GE(next, Seconds(270));
  EXPECT_LE(next, Seconds(330));
  // Over a simulated day the rate stays at the budget: 24 h / 300 s = 288 (± jitter noise).
  int captures = 0;
  for (int64_t t = 61; t < 86400 + 60; ++t) captures += sch.tick(at(t), active()).kind == SchedDecision::Kind::Capture;
  EXPECT_NEAR(captures, 288, 12);
}

TEST(Scheduler, SkipReasonsInPriorityOrder) {
  auto reason = [](SchedState s) {
    Scheduler sch(2, Seconds(0));
    const auto d = sch.tick(at(0), s);
    return d.kind == SchedDecision::Kind::Skip ? d.skipReason : std::string("capture");
  };
  auto s = active();
  EXPECT_EQ(reason(s), "capture");
  s.idle = true;
  EXPECT_EQ(reason(s), "idle");
  s.passive = true;  // a fullscreen video still gets captured
  EXPECT_EQ(reason(s), "capture");
  s.locked = true;
  EXPECT_EQ(reason(s), "locked");
  s = active();
  s.online = false;
  EXPECT_EQ(reason(s), "offline");
  s.enabled = false;
  EXPECT_EQ(reason(s), "disabled");
  s.configKnown = false;
  s.enabled = true;
  EXPECT_EQ(reason(s), "disabled");  // nothing is captured before the first config arrives
  s.paused = true;
  EXPECT_EQ(reason(s), "paused");
}

TEST(Scheduler, PullForwardNeverExceedsOneCapturePerInterval) {
  Scheduler sch(3, Seconds(0));
  ASSERT_EQ(sch.tick(at(0), active()).kind, SchedDecision::Kind::Capture);
  EXPECT_FALSE(sch.pullForward(at(100), 300));  // too soon after the last capture
  EXPECT_TRUE(sch.pullForward(at(300), 300) || *sch.nextDue() <= at(300));
  EXPECT_EQ(sch.tick(at(300), active()).kind, SchedDecision::Kind::Capture);
  EXPECT_FALSE(sch.pullForward(at(310), 300));
}
