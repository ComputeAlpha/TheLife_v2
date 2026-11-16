#include <gtest/gtest.h>

#include "core/json.h"
#include "notify/notifications.h"

using namespace thelife::collector;

namespace {
const char* kId = "0196f1c2-7a3b-7c4d-8e5f-0123456789ab";
const char* kRef = "0196f1c2-7a3b-7c4d-8e5f-aaaaaaaaaaaa";
SysTime at(int64_t s) { return SysTime(std::chrono::seconds(s)); }
}  // namespace

TEST(Notes, ParseKeepsDataOnlyAndNormalisesTheRest) {
  const auto doc = *json::parseStrict(std::string(R"({"items": [
    {"id": ")") + kId + R"(", "title": "Stand up", "body": "You have sat for 2h", "urgency": "critical",
     "action": "open_approval", "ref_id": ")" + kRef + R"(", "url": "https://evil.example", "exec": "sh"},
    {"id": "not-a-uuid", "title": "x", "body": "y"},
    {"id": ")" + kId + R"(", "title": 5, "body": "y"},
    {"id": ")" + kId + R"(", "title": "t", "body": "b", "urgency": "panic", "action": "run_command",
     "ref_id": "../../etc"}
  ]})");
  int skipped = 0;
  const auto notes = parseNotes(doc, &skipped);
  EXPECT_EQ(skipped, 2);
  ASSERT_EQ(notes.size(), 2u);
  EXPECT_EQ(notes[0].urgency, "critical");
  EXPECT_EQ(notes[0].action, "open_approval");
  EXPECT_EQ(notes[0].refId, kRef);
  EXPECT_EQ(notes[1].urgency, "normal");
  EXPECT_EQ(notes[1].action, "");
  EXPECT_EQ(notes[1].refId, "");
}

TEST(Notes, PlainTextEscapesMarkupAndCapsCharacters) {
  EXPECT_EQ(plainText("<b>hi</b> & \"bye\"", 100), "&lt;b&gt;hi&lt;/b&gt; &amp; &quot;bye&quot;");
  EXPECT_EQ(plainText("<a href='x'>", 100), "&lt;a href=&#39;x&#39;&gt;");
  EXPECT_EQ(plainText("ééééé", 3), "ééé");  // characters, not bytes
  EXPECT_EQ(plainText("line\nbreak\x07", 100), "line break ");
}

TEST(Notes, ClickUrlsAreBuiltLocallyOrNotAtAll) {
  Note n;
  n.id = kId;
  n.action = "open_chat";
  EXPECT_EQ(clickUrl("http://127.0.0.1:5000", n), "http://127.0.0.1:5000/chat");
  n.action = "open_approval";
  n.refId = kRef;
  EXPECT_EQ(clickUrl("http://127.0.0.1:5000", n), std::string("http://127.0.0.1:5000/chat#approval-") + kRef);
  EXPECT_FALSE(clickUrl("", n));  // no web UI configured
  n.action = "";
  EXPECT_FALSE(clickUrl("http://127.0.0.1:5000", n));
}

TEST(Notes, HeldWhileAwayForTenMinutesAndNeverShownTwice) {
  NoteHolder h;
  Note a;
  a.id = kId;
  EXPECT_TRUE(h.add(a, at(0)));
  EXPECT_FALSE(h.add(a, at(1)));  // the backend repeats it until acknowledged
  EXPECT_TRUE(h.due(at(60), false).empty());
  EXPECT_EQ(h.held(), 1u);
  EXPECT_EQ(h.due(at(120), true).size(), 1u);
  EXPECT_EQ(h.held(), 0u);
  Note b;
  b.id = kRef;
  h.add(b, at(1000));
  EXPECT_TRUE(h.due(at(1000 + 601), true).empty());  // expired while away: dropped (Telegram takes over)
  EXPECT_FALSE(h.add(b, at(2000)));
}
