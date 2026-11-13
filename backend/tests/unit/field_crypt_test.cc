// Field encryption primitives (backend_lld.md §9): sealing, the associated-data binding, legacy values and the KEK
// formats. No database: the DEK is a fixed test key.
#include <gtest/gtest.h>

#include <cstdlib>

#include "core/crypto.h"
#include "core/json.h"
#include "db/field_crypt.h"

using namespace thelife;

namespace {

fieldcrypt::Dek testDek(unsigned char fill = 7) { return {std::vector<unsigned char>(32, fill)}; }
const std::string kTenant = "0190a1b2-0000-7000-8000-000000000001";

std::string text(const std::vector<unsigned char>& b) { return std::string(b.begin(), b.end()); }

}  // namespace

TEST(FieldCrypt, SealedValuesRoundTripAndHideThePlaintext) {
  Json::Value notes("Call the clinic about the knee scan");
  const auto sealed = fieldcrypt::seal(testDek(), kTenant, "tasks", "notes", notes);
  EXPECT_EQ(sealed[0], 0x01);
  EXPECT_EQ(text(sealed).find("clinic"), std::string::npos);
  EXPECT_EQ(fieldcrypt::open(testDek(), kTenant, "tasks", "notes", sealed), notes);

  // A fresh nonce each time: the same value never seals to the same bytes.
  EXPECT_NE(fieldcrypt::seal(testDek(), kTenant, "tasks", "notes", notes), sealed);

  Json::Value evidence;
  evidence["window_title"] = "Quarterly report.odt";
  evidence["processes"].append("soffice");
  const auto e = fieldcrypt::seal(testDek(), kTenant, "detections", "evidence", evidence);
  EXPECT_EQ(fieldcrypt::open(testDek(), kTenant, "detections", "evidence", e), evidence);
}

TEST(FieldCrypt, AValueOpensOnlyForItsTenantColumnAndKey) {
  const auto sealed = fieldcrypt::seal(testDek(), kTenant, "tasks", "notes", Json::Value("private"));
  EXPECT_THROW(fieldcrypt::open(testDek(), "0190a1b2-0000-7000-8000-000000000002", "tasks", "notes", sealed),
               std::runtime_error);
  EXPECT_THROW(fieldcrypt::open(testDek(), kTenant, "goals", "description", sealed), std::runtime_error);
  EXPECT_THROW(fieldcrypt::open(testDek(8), kTenant, "tasks", "notes", sealed), std::runtime_error);
  auto tampered = sealed;
  tampered.back() ^= 1;
  EXPECT_THROW(fieldcrypt::open(testDek(), kTenant, "tasks", "notes", tampered), std::runtime_error);
  auto truncated = sealed;
  truncated.resize(20);
  EXPECT_THROW(fieldcrypt::open(testDek(), kTenant, "tasks", "notes", truncated), std::runtime_error);
}

TEST(FieldCrypt, LegacyValuesAreReadableAndPgHexParses) {
  std::vector<unsigned char> legacy{0x00};
  const std::string json = R"("written before 0014")";
  legacy.insert(legacy.end(), json.begin(), json.end());
  EXPECT_EQ(fieldcrypt::open(testDek(), kTenant, "tasks", "notes", legacy).asString(), "written before 0014");

  const std::vector<unsigned char> bytes{0x01, 0xab, 0x00, 0xff};
  EXPECT_EQ(fieldcrypt::fromPgHex(crypto::pgByteaHex(bytes)), bytes);
  EXPECT_THROW(fieldcrypt::fromPgHex("01ab"), std::runtime_error);
  EXPECT_THROW(fieldcrypt::fromPgHex("\\x0"), std::runtime_error);
  EXPECT_THROW(fieldcrypt::fromPgHex("\\xzz"), std::runtime_error);
}

TEST(FieldCrypt, KekIsReadAsBase64OrHexAndRejectedOtherwise) {
  unsetenv("THELIFE_KEK_FILE");
  for (const char* good : {"dGhlbGlmZS10ZXN0LWtlay1ub3QtYS1zZWNyZXQhISE=", "dGhlbGlmZS10ZXN0LWtlay1ub3QtYS1zZWNyZXQhISE",
                           "7468656c6966652d746573742d6b656b2d6e6f742d612d736563726574212121"}) {
    setenv("THELIFE_KEK", good, 1);
    EXPECT_NO_THROW(fieldcrypt::init()) << good;
    EXPECT_TRUE(fieldcrypt::ready());
  }
  for (const char* bad : {"", "c2hvcnQ=", "not base64 at all !!", "7468656c"}) {
    setenv("THELIFE_KEK", bad, 1);
    EXPECT_THROW(fieldcrypt::init(), std::runtime_error) << bad;
  }
  setenv("THELIFE_KEK_FILE", "/nonexistent/kek", 1);
  EXPECT_THROW(fieldcrypt::init(), std::runtime_error);
  unsetenv("THELIFE_KEK_FILE");
  unsetenv("THELIFE_KEK");
}

TEST(FieldCrypt, TheColumnListCoversNotesDescriptionsAndDetectionText) {
  const auto* d = fieldcrypt::columnsOf("detections");
  ASSERT_NE(d, nullptr);
  EXPECT_TRUE(d->count("summary") && d->count("evidence"));
  ASSERT_NE(fieldcrypt::columnsOf("tasks"), nullptr);
  EXPECT_EQ(fieldcrypt::columnsOf("messages"), nullptr);  // chat stays searchable (decided 2026-11-13)
}
