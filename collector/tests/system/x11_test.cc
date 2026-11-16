// XcbSource against a real X server (Xvfb): collector_lld.md §11 "x11".
#include <gtest/gtest.h>
#include <unistd.h>

#include <thread>

#include "capture/image.h"
#include "support/system.h"
#include "x11/xcb_source.h"

using namespace thelife::collector;
using namespace thelife::collector::testing;

class X11 : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { xvfb_ = new Xvfb(); }
  static void TearDownTestSuite() {
    delete xvfb_;
    xvfb_ = nullptr;
  }
  void SetUp() override {
    if (!xvfb_->ok()) GTEST_SKIP() << "Xvfb not available";
    std::string err;
    src_ = XcbSource::connect(xvfb_->display().c_str(), &err);
    ASSERT_TRUE(src_) << err;
    client_ = std::make_unique<XClient>(xvfb_->display());
  }
  // Drains until `pred(events)` or a timeout.
  bool waitEvents(const std::function<bool(const XcbSource::Events&)>& pred) {
    XcbSource::Events seen;
    return waitUntil(
        [&] {
          const auto e = src_->drain();
          seen.activeChanged |= e.activeChanged;
          seen.windowChanged |= e.windowChanged;
          return pred(seen);
        },
        std::chrono::seconds(3), std::chrono::milliseconds(20));
  }
  static Xvfb* xvfb_;
  std::unique_ptr<XcbSource> src_;
  std::unique_ptr<XClient> client_;
};
Xvfb* X11::xvfb_ = nullptr;

TEST_F(X11, ReadsTheFocusedWindowAndFollowsChanges) {
  const auto a = client_->createWindow(100, 100, 400, 300, 0x00ff00, "code", "main.cc — TheLife_v2",
                                       static_cast<uint32_t>(::getpid()));
  const auto b = client_->createWindow(2000, 100, 400, 300, 0x0000ff, "firefox", "Inbox", std::nullopt);
  client_->activate(a);
  ASSERT_TRUE(waitEvents([](const auto& e) { return e.activeChanged; }));
  ASSERT_EQ(src_->activeWindow(), a);
  auto w = src_->window(a);
  EXPECT_EQ(w.wmInstance, "code");
  EXPECT_EQ(w.wmClass, "code");
  EXPECT_EQ(w.title, "main.cc — TheLife_v2");
  EXPECT_EQ(w.pid, ::getpid());
  ASSERT_TRUE(w.geometry);
  EXPECT_EQ(w.geometry->x, 100);
  EXPECT_EQ(w.geometry->w, 400);
  EXPECT_FALSE(w.fullscreen);

  // Title changes of the watched window arrive as events; other windows' do not.
  src_->watch(a);
  client_->setTitle(b, "Inbox (1)");
  client_->setTitle(a, "util.cc — TheLife_v2");
  ASSERT_TRUE(waitEvents([](const auto& e) { return e.windowChanged; }));
  EXPECT_EQ(src_->window(a).title, "util.cc — TheLife_v2");

  client_->setFullscreen(a, true);
  EXPECT_TRUE(src_->window(a).fullscreen);

  client_->activate(b);
  ASSERT_TRUE(waitEvents([](const auto& e) { return e.activeChanged; }));
  const auto wb = src_->window(b);
  EXPECT_EQ(wb.wmClass, "firefox");
  EXPECT_EQ(wb.pid, ::getpid());  // no _NET_WM_PID: the X-Resource fallback finds the owning client
  const auto ms = src_->monitors();
  ASSERT_EQ(ms.size(), 2u);
  EXPECT_EQ(monitorOf(ms, *w.geometry), 0);
  EXPECT_EQ(monitorOf(ms, *wb.geometry), 1);
}

TEST_F(X11, TwoRandrMonitors) {
  const auto ms = src_->monitors();
  ASSERT_EQ(ms.size(), 2u);
  EXPECT_EQ(ms[0].r.x, 0);
  EXPECT_EQ(ms[0].r.w, 1920);
  EXPECT_EQ(ms[1].r.x, 1920);
  EXPECT_EQ(ms[1].r.h, 1080);
}

TEST_F(X11, GrabsMonitorPixelsAndScalesThem) {
  client_->createWindow(1920, 0, 1920, 1080, 0xff0000, "red", "red", std::nullopt);
  std::string err;
  std::optional<Image> img;
  ASSERT_TRUE(waitUntil(
      [&] {
        img = src_->grab(Rect{1920, 0, 1920, 1080}, &err);
        return img && img->px.data()[2] == 0xff;
      },
      std::chrono::seconds(3)))
      << err;
  EXPECT_EQ(img->width, 1920);
  EXPECT_EQ(img->height, 1080);
  const auto* mid = img->px.data() + (540 * 1920 + 960) * 4;
  EXPECT_EQ(mid[0], 0x00);  // B
  EXPECT_EQ(mid[1], 0x00);  // G
  EXPECT_EQ(mid[2], 0xff);  // R
  const auto scaled = downscale(std::move(*img), 1280);
  EXPECT_EQ(scaled.width, 1280);
  EXPECT_EQ(scaled.height, 720);
  const auto jpeg = encodeJpeg(scaled, 80, &err);
  ASSERT_TRUE(jpeg) << err;
  EXPECT_EQ(jpeg->data()[0], 0xFF);
}

TEST_F(X11, IdleTimeResetsOnInput) {
  client_->fakeMotion(10, 10);
  ASSERT_TRUE(waitUntil([&] { return src_->idle().msSinceInput < 500; }, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_GE(src_->idle().msSinceInput, 1000u);
  client_->fakeMotion(20, 20);
  EXPECT_TRUE(waitUntil([&] { return src_->idle().msSinceInput < 500; }, std::chrono::seconds(2)));
  EXPECT_FALSE(src_->idle().screensaverOn);
}
