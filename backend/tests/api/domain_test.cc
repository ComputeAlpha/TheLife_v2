// Behaviour beyond plain CRUD: recurring task occurrences, the merged calendar, detection state machine,
// derived skill progress, link checks, devices and API keys.
#include <gtest/gtest.h>

#include "core/json.h"
#include "support/api_client.h"

using namespace thelife::test;
using thelife::json::parse;

namespace {

class DomainApi : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (dbAvailable()) server_ = startServerWithDb().release();
  }
  static void TearDownTestSuite() {
    delete server_;
    server_ = nullptr;
  }
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    alice_ = std::make_unique<ApiClient>(*server_, createTenantWithUser());
    bob_ = std::make_unique<ApiClient>(*server_, createTenantWithUser());
  }
  std::string create(const std::string& path, const std::string& json) {
    auto r = alice_->post(path, parse(json));
    EXPECT_EQ(r.status, 201) << path << " " << r.body;
    return r.json["id"].asString();
  }
  static ServerProcess* server_;
  std::unique_ptr<ApiClient> alice_, bob_;
};
ServerProcess* DomainApi::server_ = nullptr;

// ---------------------------------------------------------------- tasks

TEST_F(DomainApi, RecurringTaskOccurrencesExpandAndMaterialize) {
  const auto id = create("/tasks", R"({"title":"stretch","area":"fitness","rrule":"FREQ=WEEKLY;BYDAY=MO,WE,FR",
                                      "rrule_dtstart":"2026-01-05T07:00:00+00:00"})");
  auto week = alice_->get("/tasks/" + id + "/occurrences?from=2026-01-05&to=2026-01-11");
  ASSERT_EQ(week.status, 200) << week.body;
  ASSERT_EQ(week.json["items"].size(), 3u);
  EXPECT_EQ(week.json["items"][0]["occurs_on"].asString(), "2026-01-05");
  EXPECT_EQ(week.json["items"][2]["occurs_on"].asString(), "2026-01-09");
  EXPECT_FALSE(week.json["items"][0]["materialized"].asBool());

  auto done = alice_->post("/tasks/" + id + "/occurrences/2026-01-07:complete", Json::Value(Json::objectValue));
  ASSERT_EQ(done.status, 200) << done.body;
  EXPECT_EQ(done.json["status"].asString(), "done");
  EXPECT_FALSE(done.json["completed_at"].isNull());
  auto skip = alice_->post("/tasks/" + id + "/occurrences/2026-01-09:skip", parse(R"({"reason":"travel"})"));
  ASSERT_EQ(skip.status, 200) << skip.body;
  EXPECT_EQ(skip.json["skip_reason"].asString(), "travel");
  // no reason → NULL, not '' (regression: nullable parameters through the coroutine exec path)
  auto bare = alice_->post("/tasks/" + id + "/occurrences/2026-01-12:skip", Json::Value(Json::objectValue));
  ASSERT_EQ(bare.status, 200) << bare.body;
  EXPECT_TRUE(bare.json["skip_reason"].isNull()) << bare.body;
  // completing a skipped one later is allowed and clears the reason
  auto redo = alice_->post("/tasks/" + id + "/occurrences/2026-01-09:complete",
                           parse(R"({"completed_at":"2026-01-09T20:00:00+00:00"})"));
  EXPECT_EQ(redo.json["completed_at"].asString(), "2026-01-09T20:00:00+00:00");
  EXPECT_TRUE(redo.json["skip_reason"].isNull());

  week = alice_->get("/tasks/" + id + "/occurrences?from=2026-01-05&to=2026-01-11");
  EXPECT_EQ(week.json["items"][1]["status"].asString(), "done");
  EXPECT_TRUE(week.json["items"][1]["materialized"].asBool());
  EXPECT_EQ(week.json["items"][2]["status"].asString(), "done");

  // not an occurrence (Tuesday), a range too large, another tenant
  EXPECT_EQ(alice_->post("/tasks/" + id + "/occurrences/2026-01-06:complete", Json::Value(Json::objectValue)).status,
            422);
  EXPECT_EQ(alice_->get("/tasks/" + id + "/occurrences?from=2026-01-01&to=2027-06-01").status, 422);
  EXPECT_EQ(bob_->get("/tasks/" + id + "/occurrences").status, 404);
  EXPECT_EQ(bob_->post("/tasks/" + id + "/occurrences/2026-01-07:complete", Json::Value(Json::objectValue)).status,
            404);
}

TEST_F(DomainApi, OneOffTasksAndRuleValidation) {
  const auto id = create("/tasks", R"({"title":"taxes","area":"personal","due_at":"2026-03-10T12:00:00+00:00"})");
  auto occ = alice_->get("/tasks/" + id + "/occurrences?from=2026-03-01&to=2026-03-31");
  ASSERT_EQ(occ.json["items"].size(), 1u) << occ.body;
  EXPECT_EQ(occ.json["items"][0]["occurs_on"].asString(), "2026-03-10");
  EXPECT_EQ(alice_->post("/tasks/" + id + "/occurrences/2026-03-10:complete", Json::Value(Json::objectValue)).status,
            409);
  auto done = alice_->patch("/tasks/" + id, parse(R"({"status":"done"})"), "\"1\"");
  EXPECT_FALSE(done.json["completed_at"].isNull());
  EXPECT_TRUE(alice_->patch("/tasks/" + id, parse(R"({"status":"open"})"), "\"2\"").json["completed_at"].isNull());

  auto bad = alice_->post("/tasks", parse(R"({"title":"x","area":"work","rrule":"FREQ=MINUTELY"})"));
  ASSERT_EQ(bad.status, 422);
  EXPECT_EQ(bad.json["errors"][0]["path"].asString(), "/rrule");
  // anchor defaults to creation time
  auto anchored = alice_->post("/tasks", parse(R"({"title":"x","area":"work","rrule":"FREQ=DAILY"})"));
  EXPECT_FALSE(anchored.json["rrule_dtstart"].isNull());
  EXPECT_EQ(alice_->get("/tasks?recurring=true").json["items"].size(), 1u);
  EXPECT_EQ(alice_->get("/tasks?due_before=2026-04-01T00:00:00Z").json["items"].size(), 1u);
}

TEST_F(DomainApi, DayStartHourDecidesTheOccurrenceDay) {
  // 02:00 local belongs to the previous day when the day starts at 04:00 (scoring_lld.md §2).
  ASSERT_EQ(alice_->patch("/me", parse(R"({"timezone":"Asia/Kolkata","day_start_hour":4})"), "").status, 200);
  EXPECT_EQ(alice_->patch("/me", parse(R"({"workdays":["MO","TU","MO"]})"), "").status, 422);  // unique days
  const auto id = create("/tasks", R"({"title":"late","area":"skill","rrule":"FREQ=DAILY;COUNT=2",
                                      "rrule_dtstart":"2026-02-09T20:30:00+00:00"})");  // 02:00 IST on the 10th
  auto occ = alice_->get("/tasks/" + id + "/occurrences?from=2026-02-08&to=2026-02-12");
  ASSERT_EQ(occ.json["items"].size(), 2u) << occ.body;
  EXPECT_EQ(occ.json["items"][0]["occurs_on"].asString(), "2026-02-09");
}

// ---------------------------------------------------------------- calendar

TEST_F(DomainApi, CalendarViewMergesSourcesInTimeOrder) {
  ASSERT_EQ(alice_->patch("/me", parse(R"({"timezone":"Europe/Berlin","day_start_hour":4})"), "").status, 200);
  // weekly 09:00 Berlin meeting across the end of DST (2026-10-25)
  create("/events", R"({"title":"standup","starts_at":"2026-10-19T07:00:00+00:00",
                        "ends_at":"2026-10-19T07:15:00+00:00","rrule":"FREQ=WEEKLY"})");
  create("/events", R"({"title":"dentist","starts_at":"2026-10-27T13:00:00+00:00",
                        "ends_at":"2026-10-27T14:00:00+00:00"})");
  create("/tasks", R"({"title":"gym","area":"fitness","rrule":"FREQ=DAILY","rrule_dtstart":"2026-10-01T06:00:00+00:00"})");
  const auto book = create("/skills/resources", R"({"kind":"book","title":"Dune","total_pages":400})");
  create("/skills/sessions", R"({"resource_id":")" + book + R"(","started_at":"2026-10-26T19:00:00+00:00",
                                 "ended_at":"2026-10-26T20:00:00+00:00","page_from":1,"page_to":40})");
  const auto game = create("/entertainment/items", R"({"kind":"game","title":"Factorio"})");
  create("/entertainment/slots", R"({"item_id":")" + game + R"(","starts_at":"2026-10-27T18:00:00+00:00",
                                    "ends_at":"2026-10-27T20:00:00+00:00"})");

  auto cal = alice_->get("/calendar?from=2026-10-26T00:00:00Z&to=2026-10-28T00:00:00Z");
  ASSERT_EQ(cal.status, 200) << cal.body;
  EXPECT_FALSE(cal.json["truncated"].asBool());
  std::map<std::string, int> kinds;
  std::string prev;
  for (const auto& e : cal.json["items"]) {
    ++kinds[e["kind"].asString()];
    EXPECT_GE(e["starts_at"].asString(), prev);
    prev = e["starts_at"].asString();
    if (e["title"].asString() == "standup") {
      EXPECT_EQ(e["starts_at"].asString(), "2026-10-26T08:00:00+00:00");  // still 09:00 local, now UTC+1
      EXPECT_EQ(e["occurs_on"].asString(), "2026-10-26");
    }
    if (e["kind"].asString() == "entertainment_slot") {
      EXPECT_EQ(e["title"].asString(), "Factorio");
    }
    if (e["kind"].asString() == "skill_session") {
      EXPECT_EQ(e["title"].asString(), "Dune");
    }
  }
  EXPECT_EQ(kinds["event"], 2);
  EXPECT_EQ(kinds["entertainment_slot"], 1);
  EXPECT_EQ(kinds["skill_session"], 1);
  EXPECT_GE(kinds["task_occurrence"], 2);  // gym on the 26th and 27th (plus the overlapping 25th/28th day blocks)

  EXPECT_EQ(alice_->get("/calendar?from=2026-10-28T00:00:00Z&to=2026-10-26T00:00:00Z").status, 422);
  EXPECT_EQ(alice_->get("/calendar?from=2026-01-01T00:00:00Z&to=2026-06-01T00:00:00Z").status, 422);
  EXPECT_EQ(alice_->get("/calendar").status, 200);  // defaults to the next 7 days
  EXPECT_EQ(bob_->get("/calendar?from=2026-10-26T00:00:00Z&to=2026-10-28T00:00:00Z").json["items"].size(), 0u);
}

TEST_F(DomainApi, LinksMustPointAtLiveItemsOfTheSameTenant) {
  const auto goal = create("/goals", R"({"title":"g","area":"skill","horizon":"year"})");
  auto ok = alice_->post("/events", parse(R"({"title":"plan","starts_at":"2026-01-01T09:00:00Z",
      "ends_at":"2026-01-01T10:00:00Z","link_type":"goal","link_id":")" + goal + "\"}"));
  EXPECT_EQ(ok.status, 201) << ok.body;
  auto foreign = bob_->post("/events", parse(R"({"title":"x","starts_at":"2026-01-01T09:00:00Z",
      "ends_at":"2026-01-01T10:00:00Z","link_type":"goal","link_id":")" + goal + "\"}"));
  ASSERT_EQ(foreign.status, 422) << foreign.body;
  EXPECT_NE(foreign.json["errors"][0]["message"].asString().find("link_id"), std::string::npos);
  EXPECT_EQ(alice_->post("/activities", parse(R"({"area":"skill","title":"x","started_at":"2026-01-01T09:00:00Z",
      "ended_at":"2026-01-01T10:00:00Z","link_type":"task","link_id":")" + goal + "\"}")).status,
            422);  // right tenant, wrong type
}

// ---------------------------------------------------------------- skills and work

TEST_F(DomainApi, SkillProgressIsDerivedFromSessions) {
  const auto book = create("/skills/resources", R"({"kind":"book","title":"b","total_pages":200})");
  create("/skills/sessions", R"({"resource_id":")" + book + R"(","started_at":"2026-01-01T20:00:00Z",
                                 "ended_at":"2026-01-01T21:00:00Z","page_from":1,"page_to":50})");
  auto r = alice_->get("/skills/resources/" + book);
  EXPECT_EQ(r.json["progress"]["pages_done"].asInt(), 50);
  EXPECT_DOUBLE_EQ(r.json["progress"]["percent"].asDouble(), 25.0);
  // a book session cannot record course positions
  EXPECT_EQ(alice_->post("/skills/sessions", parse(R"({"resource_id":")" + book + R"(","started_at":"2026-01-02T20:00:00Z",
                                                     "ended_at":"2026-01-02T21:00:00Z","pos_from_s":0,"pos_to_s":60})"))
                .status,
            422);

  const auto course = create("/skills/resources", R"({"kind":"course","title":"c","total_duration_s":3000})");
  create("/skills/sections", R"({"resource_id":")" + course + R"(","ordinal":0,"title":"one","duration_s":1000})");
  const auto s2 = create("/skills/sections", R"({"resource_id":")" + course + R"(","ordinal":1,"title":"two"})");
  create("/skills/sessions", R"({"resource_id":")" + course + R"(","section_id":")" + s2 + R"(",
                                 "started_at":"2026-01-01T20:00:00Z","ended_at":"2026-01-01T21:00:00Z",
                                 "pos_from_s":0,"pos_to_s":500})");
  r = alice_->get("/skills/resources/" + course);
  EXPECT_EQ(r.json["progress"]["position_s"].asInt(), 1500);
  EXPECT_DOUBLE_EQ(r.json["progress"]["percent"].asDouble(), 50.0);
  // duplicate ordinal, section of another resource, sections on a book
  EXPECT_EQ(alice_->post("/skills/sections", parse(R"({"resource_id":")" + course + R"(","ordinal":1,"title":"dup"})"))
                .status,
            409);
  EXPECT_EQ(alice_->post("/skills/sections", parse(R"({"resource_id":")" + book + R"(","ordinal":0,"title":"x"})"))
                .status,
            422);
}

TEST_F(DomainApi, DeliverableDeliveredAtFollowsStatus) {
  const auto project = create("/work/projects", R"({"title":"p"})");
  const auto d = create("/work/deliverables", R"({"project_id":")" + project + R"(","title":"spec"})");
  auto r = alice_->patch("/work/deliverables/" + d, parse(R"({"status":"delivered"})"), "\"1\"");
  EXPECT_FALSE(r.json["delivered_at"].isNull());
  EXPECT_EQ(alice_->post("/work/responsibilities", parse(R"({"role_id":")" + project + R"(","title":"x"})")).status,
            422);  // a project id is not a role id
  EXPECT_EQ(alice_->post("/work/responsibilities",
                         parse(R"({"role_id":")" + create("/work/roles", R"({"title":"r"})") +
                               R"(","title":"x","cadence":"every monday"})"))
                .status,
            422);
}

// ---------------------------------------------------------------- detections

TEST_F(DomainApi, DetectionStateMachineAndHistory) {
  auto ext = alice_->post("/detections", parse(R"({"source":"external","started_at":"2026-01-01T09:00:00Z",
      "ended_at":"2026-01-01T10:00:00Z","area":"work","category":"coding","summary":"editor","confidence":0.7})"));
  ASSERT_EQ(ext.status, 201) << ext.body;
  EXPECT_EQ(ext.json["status"].asString(), "detected");
  const auto id = ext.json["id"].asString();

  EXPECT_EQ(alice_->post("/detections/" + id + ":confirm", Json::Value(Json::objectValue)).json["status"].asString(),
            "confirmed");
  EXPECT_EQ(alice_->post("/detections/" + id + ":confirm", Json::Value(Json::objectValue)).status, 409);
  EXPECT_EQ(alice_->post("/detections/" + id + ":dispute", parse(R"({})")).status, 422);  // reason required
  auto disputed = alice_->post("/detections/" + id + ":dispute", parse(R"({"reason":"was a meeting"})"));
  EXPECT_EQ(disputed.json["status"].asString(), "disputed");

  auto claim = alice_->post("/detections/" + id + ":claim",
                            parse(R"({"area":"work","category":"meeting","summary":"design review"})"));
  ASSERT_EQ(claim.status, 201) << claim.body;
  EXPECT_EQ(claim.json["source"].asString(), "user");
  EXPECT_EQ(claim.json["status"].asString(), "confirmed");
  EXPECT_EQ(claim.json["supersedes_id"].asString(), id);
  EXPECT_EQ(claim.json["started_at"], ext.json["started_at"]);  // inherited window
  EXPECT_EQ(alice_->post("/detections/" + id + ":claim", parse(R"({"area":"work","category":"x","summary":"y"})"))
                .status,
            409);
  EXPECT_EQ(alice_->post("/detections/" + id + ":confirm", Json::Value(Json::objectValue)).status, 409);

  auto hist = alice_->get("/detections/" + id + "/history");
  ASSERT_EQ(hist.status, 200) << hist.body;
  EXPECT_EQ(hist.json["detection"]["status"].asString(), "superseded");
  ASSERT_EQ(hist.json["events"].size(), 3u);
  EXPECT_EQ(hist.json["events"][1]["reason"].asString(), "was a meeting");
  EXPECT_EQ(hist.json["events"][2]["to_status"].asString(), "superseded");
  EXPECT_EQ(hist.json["events"][0]["actor_kind"].asString(), "user");
  ASSERT_EQ(hist.json["chain"].size(), 2u);
  EXPECT_EQ(hist.json["chain"][1]["id"], claim.json["id"]);

  // user-stated detections start confirmed; vision/process come only from devices
  auto stated = alice_->post("/detections", parse(R"({"source":"user","started_at":"2026-01-01T11:00:00Z",
      "ended_at":"2026-01-01T12:00:00Z","area":"fitness","category":"run","summary":"ran 5k"})"));
  EXPECT_EQ(stated.json["status"].asString(), "confirmed");
  EXPECT_EQ(alice_->post("/detections", parse(R"({"source":"vision","started_at":"2026-01-01T11:00:00Z",
      "ended_at":"2026-01-01T12:00:00Z","area":"work","category":"x","summary":"y"})")).status,
            422);
  // no update or delete surface; foreign tenant sees nothing
  const auto patch = alice_->patch("/detections/" + id, parse(R"({"summary":"x"})"), "\"1\"").status;
  EXPECT_TRUE(patch == 404 || patch == 405) << patch;
  EXPECT_EQ(bob_->post("/detections/" + id + ":confirm", Json::Value(Json::objectValue)).status, 404);
  EXPECT_EQ(bob_->get("/detections/" + id + "/history").status, 404);
  EXPECT_EQ(alice_->get("/detections?status=superseded").json["items"].size(), 1u);
}

// ---------------------------------------------------------------- devices and API keys

TEST_F(DomainApi, DeviceRegistrationAndRevocation) {
  auto reg = alice_->post("/devices", parse(R"({"name":"desk","platform":"linux-x11"})"));
  ASSERT_EQ(reg.status, 201) << reg.body;
  const auto token = reg.json["token"].asString();
  EXPECT_EQ(token.rfind("tld_", 0), 0u);
  EXPECT_FALSE(reg.json["device"].isMember("config"));
  const auto id = reg.json["device"]["id"].asString();
  EXPECT_EQ(alice_->get("/devices").json["items"].size(), 1u);

  const ApiClient device(*server_, alice_->user(), token);
  EXPECT_EQ(device.get("/goals").status, 403);  // valid token, wrong surface

  EXPECT_EQ(bob_->call("DELETE", "/devices/" + id).status, 404);
  EXPECT_EQ(alice_->call("DELETE", "/devices/" + id).status, 204);
  EXPECT_EQ(alice_->call("DELETE", "/devices/" + id).status, 204);  // idempotent
  EXPECT_FALSE(alice_->get("/devices/" + id).json["revoked_at"].isNull());
  EXPECT_EQ(device.get("/goals").status, 401);  // token died with the device

  // a response carrying a secret is never stored for replay
  EXPECT_EQ(alice_->post("/devices", parse(R"({"name":"x","platform":"linux-x11"})"), "idem-key-1").status, 422);
}

TEST_F(DomainApi, ApiKeysAreScopedAndRevocable) {
  auto bad = alice_->post("/api-keys", parse(R"({"name":"k","scopes":["api_keys:write"]})"));
  EXPECT_EQ(bad.status, 422);  // a key can never mint keys
  EXPECT_EQ(alice_->post("/api-keys", parse(R"({"name":"k","scopes":["tasks:read"],
                                              "expires_at":"2020-01-01T00:00:00Z"})")).status,
            422);
  auto issued = alice_->post("/api-keys", parse(R"({"name":"phone","scopes":["tasks:read","tasks:write"]})"));
  ASSERT_EQ(issued.status, 201) << issued.body;
  const auto token = issued.json["token"].asString();
  EXPECT_EQ(token.rfind("tlk_", 0), 0u);
  EXPECT_EQ(issued.json["api_key"]["token_prefix"].asString(), token.substr(0, 8));
  EXPECT_FALSE(issued.json["api_key"].isMember("token_hash"));
  const auto id = issued.json["api_key"]["id"].asString();

  const ApiClient key(*server_, alice_->user(), token);
  auto t = key.post("/tasks", parse(R"({"title":"from phone","area":"personal"})"));
  ASSERT_EQ(t.status, 201) << t.body;
  EXPECT_EQ(t.json["created_by"]["kind"].asString(), "external");
  EXPECT_EQ(key.get("/goals").status, 403);
  EXPECT_EQ(key.get("/api-keys").status, 403);

  auto listed = alice_->get("/api-keys");
  ASSERT_EQ(listed.json["items"].size(), 1u);
  EXPECT_EQ(bob_->get("/api-keys").json["items"].size(), 0u);
  EXPECT_EQ(bob_->call("DELETE", "/api-keys/" + id).status, 404);
  EXPECT_EQ(alice_->call("DELETE", "/api-keys/" + id).status, 204);
  EXPECT_EQ(key.get("/tasks").status, 401);
  EXPECT_FALSE(alice_->get("/api-keys/" + id).json["revoked_at"].isNull());
}

}  // namespace
