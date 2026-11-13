// Field encryption end to end (backend_lld.md §9, migration 0014): what the database holds is sealed, the API
// returns plaintext, detections and their claims go through the SQL functions sealed, values written before the
// migration are read and then sealed by fieldcrypt.reseal, and a server with another KEK cannot read them.
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "core/json.h"
#include "db/field_crypt.h"
#include "support/api_client.h"

using namespace thelife;
using namespace thelife::test;

namespace {

Json::Value J(const std::string& t) { return json::parse(t); }

drogon::orm::Result sql(const std::string& tenant, const std::string& q, const std::vector<std::string>& args = {}) {
  auto tx = appDb()->newTransaction();
  tx->execSqlSync("SELECT set_config('app.tenant_id', $1, true)", tenant);
  auto r = args.empty() ? tx->execSqlSync(q) : args.size() == 1 ? tx->execSqlSync(q, args[0])
                                                                : tx->execSqlSync(q, args[0], args[1]);
  std::promise<bool> committed;
  tx->setCommitCallback([&committed](bool ok) { committed.set_value(ok); });
  tx.reset();
  EXPECT_TRUE(committed.get_future().get());
  return r;
}

// The stored bytes of one column as hex text (no "\x").
std::string stored(const std::string& tenant, const std::string& table, const std::string& col, const std::string& id) {
  const auto r = sql(tenant, "SELECT coalesce(encode(" + col + ", 'hex'), '') AS h FROM " + table + " WHERE id = $1::uuid", {id});
  return r.empty() ? "" : r[0]["h"].as<std::string>();
}

std::string hexOf(const std::string& s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) out += {kHex[c >> 4], kHex[c & 15]};
  return out;
}

template <typename F>
bool eventually(F cond, int ms = 10000) {
  for (int i = 0; i < ms / 50; ++i) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return cond();
}

class Encryption : public ::testing::Test {
 protected:
  void SetUp() override {
    THELIFE_SKIP_WITHOUT_DB();
    user = createTenantWithUser();
  }
  void start(std::map<std::string, std::string> env = {}) {
    server = startServerWithDb(std::move(env));
    api = std::make_unique<ApiClient>(*server, user);
  }
  TestUser user;
  std::unique_ptr<ServerProcess> server;
  std::unique_ptr<ApiClient> api;
};

}  // namespace

TEST_F(Encryption, StoredNotesAreSealedAndTheApiReturnsPlaintext) {
  start();
  const std::string note = "Ask Dr. Rao about the knee MRI results";
  auto t = api->post("/tasks", J(R"({"title":"Clinic call","area":"personal","notes":")" + note + R"("})"));
  ASSERT_EQ(t.status, 201) << t.body;
  EXPECT_EQ(t.json["notes"].asString(), note);
  const auto id = t.json["id"].asString();

  const auto raw = stored(user.tenantId, "tasks", "notes", id);
  EXPECT_EQ(raw.substr(0, 2), "01");                                       // sealed format
  EXPECT_EQ(raw.find(hexOf("knee")), std::string::npos) << "plaintext in the database";
  EXPECT_EQ(sql(user.tenantId, "SELECT count(*) AS n FROM tenant_keys")[0]["n"].as<int>(), 1);

  EXPECT_EQ(api->get("/tasks/" + id).json["notes"].asString(), note);
  bool listed = false;
  const auto list = api->get("/tasks").json;
  for (const auto& i : list["items"]) listed |= i["notes"].asString() == note;
  EXPECT_TRUE(listed);

  // Updating seals the new value; other fields and null stay as they are.
  auto u = api->patch("/tasks/" + id, J(R"({"notes":"Rescheduled to Friday"})"), "\"" + t.json["version"].asString() + "\"");
  ASSERT_EQ(u.status, 200) << u.body;
  EXPECT_EQ(u.json["notes"].asString(), "Rescheduled to Friday");
  EXPECT_NE(stored(user.tenantId, "tasks", "notes", id), raw);
  auto cleared = api->patch("/tasks/" + id, J(R"({"notes":null})"), "\"" + u.json["version"].asString() + "\"");
  ASSERT_EQ(cleared.status, 200) << cleared.body;
  EXPECT_TRUE(cleared.json["notes"].isNull());
  EXPECT_EQ(stored(user.tenantId, "tasks", "notes", id), "");

  auto g = api->post("/goals", J(R"({"title":"Run a half marathon","area":"fitness","horizon":"year",
                                     "description":"Sub 2 hours, after the knee heals"})"));
  ASSERT_EQ(g.status, 201) << g.body;
  EXPECT_EQ(api->get("/goals/" + g.json["id"].asString()).json["description"].asString(),
            "Sub 2 hours, after the knee heals");
  EXPECT_EQ(stored(user.tenantId, "goals", "description", g.json["id"].asString()).substr(0, 2), "01");
}

TEST_F(Encryption, DetectionTextIsSealedThroughCreateClaimAndHistory) {
  start();
  auto d = api->post("/detections", J(R"({"source":"external","started_at":"2026-01-01T09:00:00Z",
      "ended_at":"2026-01-01T10:00:00Z","area":"work","category":"coding","summary":"Editing payroll.xlsx",
      "evidence":{"window_title":"payroll.xlsx - LibreOffice Calc"}})"));
  ASSERT_EQ(d.status, 201) << d.body;
  const auto id = d.json["id"].asString();
  EXPECT_EQ(d.json["evidence"]["window_title"].asString(), "payroll.xlsx - LibreOffice Calc");
  EXPECT_EQ(stored(user.tenantId, "detections", "summary", id).find(hexOf("payroll")), std::string::npos);
  EXPECT_EQ(stored(user.tenantId, "detections", "evidence", id).find(hexOf("payroll")), std::string::npos);

  auto claim = api->post("/detections/" + id + ":claim",
                         J(R"({"area":"work","category":"meeting","summary":"Budget review with finance"})"));
  ASSERT_EQ(claim.status, 201) << claim.body;
  EXPECT_EQ(claim.json["summary"].asString(), "Budget review with finance");
  const auto newId = claim.json["id"].asString();
  EXPECT_EQ(stored(user.tenantId, "detections", "summary", newId).substr(0, 2), "01");

  auto hist = api->get("/detections/" + id + "/history");
  ASSERT_EQ(hist.status, 200) << hist.body;
  ASSERT_EQ(hist.json["chain"].size(), 2u);
  EXPECT_EQ(hist.json["chain"][0]["summary"].asString(), "Editing payroll.xlsx");
  EXPECT_EQ(hist.json["chain"][1]["summary"].asString(), "Budget review with finance");

  // A dispute's outbox event names the category, never the sealed summary.
  auto other = api->post("/detections", J(R"({"source":"external","started_at":"2026-01-02T09:00:00Z",
      "ended_at":"2026-01-02T10:00:00Z","area":"entertainment","category":"video","summary":"Watching a thriller"})"));
  ASSERT_EQ(api->post("/detections/" + other.json["id"].asString() + ":dispute", J(R"({"reason":"was a lecture"})"))
                .json["status"].asString(),
            "disputed");
  const auto ev = sql(user.tenantId, "SELECT payload::text AS p FROM events WHERE kind = 'detection.disputed'");
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0]["p"].as<std::string>().find("thriller"), std::string::npos);
  EXPECT_NE(ev[0]["p"].as<std::string>().find("video"), std::string::npos);
}

TEST_F(Encryption, LegacyValuesAreReadThenSealedByTheResealJob) {
  start({{"THELIFE_JOBS", "on"}, {"THELIFE_JOBS_TENANT", user.tenantId}, {"THELIFE_JOBS_HOUSEKEEPING", "off"},
         {"THELIFE_JOBS_POLL_MS", "100"}});
  auto t = api->post("/tasks", J(R"({"title":"Old task","area":"work"})"));
  ASSERT_EQ(t.status, 201) << t.body;
  const auto taskId = t.json["id"].asString();
  // What migration 0014 made of a value written before it: 0x00 | JSON text.
  sql(user.tenantId,
      "UPDATE tasks SET notes = '\\x00'::bytea || convert_to(to_jsonb('from before 0014'::text)::text, 'UTF8') "
      "WHERE id = $1::uuid",
      {taskId});
  const auto detId = sql(user.tenantId,
      "INSERT INTO detections (id, tenant_id, created_by_kind, source, started_at, ended_at, area, category, summary, "
      "evidence) VALUES (app.uuid_v7(), $1::uuid, 'system', 'external', now() - interval '1 hour', now(), 'work', "
      "'coding', '\\x00'::bytea || convert_to('\"legacy summary\"', 'UTF8'), "
      "'\\x00'::bytea || convert_to('{\"window_title\":\"legacy title\"}', 'UTF8')) RETURNING id::text AS id",
      {user.tenantId})[0]["id"].as<std::string>();

  EXPECT_EQ(api->get("/tasks/" + taskId).json["notes"].asString(), "from before 0014");
  EXPECT_EQ(api->get("/detections/" + detId).json["summary"].asString(), "legacy summary");

  sql(user.tenantId, "INSERT INTO ops.jobs (id, tenant_id, kind) VALUES (app.uuid_v7(), $1::uuid, 'fieldcrypt.reseal')",
      {user.tenantId});
  ASSERT_TRUE(eventually([&] {
    return stored(user.tenantId, "tasks", "notes", taskId).substr(0, 2) == "01" &&
           stored(user.tenantId, "detections", "summary", detId).substr(0, 2) == "01" &&
           stored(user.tenantId, "detections", "evidence", detId).substr(0, 2) == "01";
  }));
  EXPECT_EQ(api->get("/tasks/" + taskId).json["notes"].asString(), "from before 0014");
  const auto det = api->get("/detections/" + detId).json;
  EXPECT_EQ(det["summary"].asString(), "legacy summary");
  EXPECT_EQ(det["evidence"]["window_title"].asString(), "legacy title");

  // The reseal path is narrow: detections stay append-only for everything else.
  EXPECT_ANY_THROW(sql(user.tenantId, "UPDATE detections SET category = 'x' WHERE id = $1::uuid", {detId}));
}

TEST_F(Encryption, AServerWithAnotherKekCannotReadTheData) {
  start();
  auto t = api->post("/tasks", J(R"({"title":"Secret","area":"personal","notes":"under the mattress"})"));
  ASSERT_EQ(t.status, 201) << t.body;
  // 32 other bytes ("another-test-kek-of-32-bytes!!!!").
  auto other = startServerWithDb({{"THELIFE_KEK", "YW5vdGhlci10ZXN0LWtlay1vZi0zMi1ieXRlcyEhISE="}});
  ApiClient intruder(*other, user);
  const auto r = intruder.get("/tasks/" + t.json["id"].asString());
  EXPECT_EQ(r.status, 500);
  EXPECT_EQ(r.body.find("mattress"), std::string::npos);
  EXPECT_EQ(intruder.get("/tasks/" + t.json["id"].asString()).body.find("mattress"), std::string::npos);
}
