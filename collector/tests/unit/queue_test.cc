#include <gtest/gtest.h>
#include <sys/stat.h>

#include "core/json.h"
#include "net/backoff.h"
#include "net/uploader.h"
#include "queue/queue.h"
#include "support/fakes.h"

using namespace thelife::collector;
using namespace thelife::collector::testing;

namespace {

Json::Value obj(const char* k, int v) {
  Json::Value j;
  j[k] = v;
  return j;
}

SysTime at(int64_t s) { return SysTime(std::chrono::seconds(s)); }

}  // namespace

TEST(Queue, SequencesStartFromTheClockAndAreNeverReused) {
  TempDir d;
  const auto before = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  Queue q(d.str() + "/queue.db");
  const auto a = q.push("window", at(1000), obj("n", 1));
  const auto b = q.push("window", at(1001), obj("n", 2));
  EXPECT_GT(a, before);  // a recreated queue cannot collide with sequences the backend already holds
  EXPECT_EQ(b, a + 1);
  q.ack({a, b});
  EXPECT_EQ(q.depth(), 0);
  EXPECT_EQ(q.push("window", at(1002), obj("n", 3)), b + 1);  // AUTOINCREMENT: deleted seqs are not reused

  struct stat st{};
  ASSERT_EQ(::stat((d.str() + "/queue.db").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST(Queue, SurvivesReopenAndSharesMetadata) {
  TempDir d;
  int64_t last;
  {
    Queue q(d.str() + "/queue.db");
    last = q.push("idle", at(5), obj("n", 1));
    q.setMeta("paused_until", "2030-01-01T00:00:00.000Z");
  }
  Queue q(d.str() + "/queue.db");
  const auto rows = q.peek(10);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].seq, last);
  EXPECT_EQ(rows[0].kind, "idle");
  EXPECT_EQ(rows[0].capturedAt, "1970-01-01T00:00:05.000Z");
  EXPECT_EQ(rows[0].payload, R"({"n":1})");
  EXPECT_EQ(q.meta("paused_until"), "2030-01-01T00:00:00.000Z");
  q.deleteMeta("paused_until");
  EXPECT_FALSE(q.meta("paused_until"));
}

TEST(Queue, CapDropsOldestRowsAndRecordsAGap) {
  TempDir d;
  Queue q(d.str() + "/queue.db");
  const auto now = std::chrono::system_clock::now();
  std::vector<int64_t> seqs;
  for (int i = 0; i < 3; ++i) seqs.push_back(q.push("window", now - std::chrono::hours(24 * 8), obj("old", i)));
  for (int i = 0; i < 10; ++i) seqs.push_back(q.push("window", now, obj("new", i)));
  // Age first: the three week-old rows go.
  EXPECT_EQ(q.enforceCap(now, std::chrono::hours(24 * 7), 1000), 3);
  auto rows = q.peek(100);
  ASSERT_EQ(rows.size(), 11u);
  EXPECT_EQ(rows.back().kind, "gap");
  const auto gap = *json::parseStrict(rows.back().payload);
  EXPECT_EQ(gap["from_seq"].asInt64(), seqs[0]);
  EXPECT_EQ(gap["to_seq"].asInt64(), seqs[2]);
  EXPECT_EQ(gap["reason"].asString(), "queue_expired");
  // Then size: at most 5 rows remain, the newest gap among them.
  EXPECT_GT(q.enforceCap(now, std::chrono::hours(24 * 7), 5), 0);
  rows = q.peek(100);
  EXPECT_EQ(rows.size(), 5u);
  EXPECT_EQ(json::parseStrict(rows.back().payload).value()["reason"].asString(), "queue_full");
}

TEST(Uploader, SendsABatchAndDeletesOnlyAcknowledgedRows) {
  TempDir d;
  Queue q(d.str() + "/queue.db");
  const auto a = q.push("window", at(10), obj("n", 1));
  const auto b = q.push("idle", at(11), obj("n", 2));
  const auto c = q.push("idle", at(12), obj("n", 3));
  FakeHttp http;
  http.respond = [&](const HttpRequest&) {
    // c is neither accepted nor a duplicate (as if the backend had cut the batch short): it stays queued.
    return response(200, "{\"accepted\": [" + std::to_string(a) + "], \"duplicates\": [" + std::to_string(b) +
                             ", 42]}");
  };
  Uploader up(q, http, "inst");
  const auto r = up.uploadOnce(at(20));
  EXPECT_EQ(r.kind, UploadResult::Kind::Uploaded);
  EXPECT_EQ(r.sent, 3);
  EXPECT_EQ(r.acked, 2);
  ASSERT_EQ(http.requests.size(), 1u);
  const auto& req = http.requests[0];
  EXPECT_EQ(req.method, "POST");
  EXPECT_EQ(req.path, "/ingest/observations");
  ASSERT_EQ(req.headers.size(), 1u);
  EXPECT_EQ(req.headers[0].first, "Idempotency-Key");
  EXPECT_EQ(req.headers[0].second, "inst:" + std::to_string(a) + "-" + std::to_string(c));
  const auto body = *json::parseStrict(req.body);
  ASSERT_EQ(body["observations"].size(), 3u);
  EXPECT_EQ(body["observations"][0]["client_seq"].asInt64(), a);
  EXPECT_EQ(body["observations"][0]["captured_at"].asString(), "1970-01-01T00:00:10.000Z");
  EXPECT_EQ(body["observations"][0]["kind"].asString(), "window");
  EXPECT_EQ(body["observations"][0]["payload"]["n"].asInt(), 1);
  const auto left = q.peek(10);
  ASSERT_EQ(left.size(), 1u);
  EXPECT_EQ(left[0].seq, c);
}

TEST(Uploader, KeepsRowsWhenTheBackendIsDownOrBusy) {
  TempDir d;
  Queue q(d.str() + "/queue.db");
  q.push("window", at(10), obj("n", 1));
  FakeHttp http;
  Uploader up(q, http, "inst");
  http.respond = [](const HttpRequest&) {
    HttpResponse r;
    r.error = "Couldn't connect to server";
    return r;
  };
  auto r = up.uploadOnce(at(20));
  EXPECT_EQ(r.kind, UploadResult::Kind::Retry);
  http.respond = [](const HttpRequest&) { return response(503, "{}", {{"retry-after", "120"}}); };
  r = up.uploadOnce(at(20));
  EXPECT_EQ(r.kind, UploadResult::Kind::Retry);
  EXPECT_EQ(r.retryAfterS, 120);
  http.respond = [](const HttpRequest&) { return response(401); };
  EXPECT_EQ(up.uploadOnce(at(20)).kind, UploadResult::Kind::Unauthorized);
  EXPECT_EQ(q.depth(), 1);
}

TEST(Uploader, DropsRowsTheBackendRejectsAsInvalidWithAGap) {
  TempDir d;
  Queue q(d.str() + "/queue.db");
  const auto a = q.push("window", at(10), obj("n", 1));
  const auto b = q.push("window", at(11), obj("bad", 1));
  const auto c = q.push("window", at(12), obj("n", 3));
  FakeHttp http;
  http.respond = [](const HttpRequest&) {
    return response(422, R"({"status": 422, "errors": [{"path": "/observations/1/payload/bad", "message": "x"}]})");
  };
  Uploader up(q, http, "inst");
  EXPECT_EQ(up.uploadOnce(at(20)).kind, UploadResult::Kind::Rejected);
  const auto rows = q.peek(10);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].seq, a);
  EXPECT_EQ(rows[1].seq, c);
  EXPECT_EQ(rows[2].kind, "gap");
  const auto gap = *json::parseStrict(rows[2].payload);
  EXPECT_EQ(gap["from_seq"].asInt64(), b);
  EXPECT_EQ(gap["reason"].asString(), "corrupt");
}

TEST(Uploader, EmptyQueueSendsNothing) {
  TempDir d;
  Queue q(d.str() + "/queue.db");
  FakeHttp http;
  Uploader up(q, http, "inst");
  EXPECT_EQ(up.uploadOnce(at(1)).kind, UploadResult::Kind::Empty);
  EXPECT_TRUE(http.requests.empty());
}

TEST(Backoff, GrowsWithFullJitterUpToTheCap) {
  Backoff b(7);
  int64_t maxSeen = 0;
  for (int i = 0; i < 40; ++i) {
    const auto s = b.next().count();
    EXPECT_GE(s, 1);
    EXPECT_LE(s, 300);
    if (i == 0) {
      EXPECT_LE(s, 2);
    }
    if (i == 1) {
      EXPECT_LE(s, 4);
    }
    maxSeen = std::max(maxSeen, s);
  }
  EXPECT_GT(maxSeen, 100);
  b.reset();
  EXPECT_LE(b.next().count(), 2);
}
