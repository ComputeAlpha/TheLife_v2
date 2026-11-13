#include "db/field_crypt.h"

#include <sodium.h>
#include <trantor/utils/Logger.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "core/crypto.h"
#include "core/env.h"
#include "core/json.h"
#include "jobs/worker.h"

namespace thelife::fieldcrypt {

namespace {

using drogon::Task;

constexpr unsigned char kSealed = 0x01;
constexpr unsigned char kLegacy = 0x00;
constexpr std::size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr std::size_t kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr std::size_t kKey = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;

struct State {
  std::mutex mu;
  bool ready = false;
  std::vector<unsigned char> kek;
  std::string kekId;
  std::map<std::string, Dek> deks;  // tenant → DEK
};

State& state() {
  static State s;
  return s;
}

std::vector<unsigned char> decodeKey(std::string text, const std::string& from) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  while (!text.empty() && text.front() == ' ') text.erase(0, 1);
  std::vector<unsigned char> out(kKey);
  size_t len = 0;
  if (text.size() == 2 * kKey &&
      sodium_hex2bin(out.data(), out.size(), text.data(), text.size(), nullptr, &len, nullptr) == 0 && len == kKey) {
    return out;
  }
  for (int variant : {sodium_base64_VARIANT_ORIGINAL, sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
                      sodium_base64_VARIANT_URLSAFE, sodium_base64_VARIANT_URLSAFE_NO_PADDING}) {
    std::vector<unsigned char> buf(kKey + 3);
    if (sodium_base642bin(buf.data(), buf.size(), text.data(), text.size(), nullptr, &len, nullptr, variant) == 0 &&
        len == kKey) {
      buf.resize(kKey);
      sodium_memzero(text.data(), text.size());
      return buf;
    }
  }
  throw std::runtime_error(from + " must hold 32 bytes as base64 or 64 hex characters "
                                  "(generate one with: openssl rand -base64 32)");
}

std::string associated(const std::string& tenantId, const std::string& table, const std::string& column) {
  return tenantId + "|" + table + "." + column;
}

std::vector<unsigned char> aeadSeal(const std::vector<unsigned char>& key, const std::string& ad,
                                    const unsigned char* msg, std::size_t len) {
  std::vector<unsigned char> out(1 + kNonce + len + kTag);
  out[0] = kSealed;
  randombytes_buf(out.data() + 1, kNonce);
  unsigned long long clen = 0;
  crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + 1 + kNonce, &clen, msg, len,
                                             reinterpret_cast<const unsigned char*>(ad.data()), ad.size(), nullptr,
                                             out.data() + 1, key.data());
  out.resize(1 + kNonce + clen);
  return out;
}

std::vector<unsigned char> aeadOpen(const std::vector<unsigned char>& key, const std::string& ad,
                                    const std::vector<unsigned char>& in) {
  if (in.size() < 1 + kNonce + kTag || in[0] != kSealed) throw std::runtime_error("not a sealed value");
  std::vector<unsigned char> out(in.size() - 1 - kNonce - kTag);
  unsigned long long mlen = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(out.data(), &mlen, nullptr, in.data() + 1 + kNonce,
                                                 in.size() - 1 - kNonce,
                                                 reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
                                                 in.data() + 1, key.data()) != 0) {
    throw std::runtime_error("sealed value does not authenticate (wrong key, tenant or column)");
  }
  out.resize(mlen);
  return out;
}

std::string wrapAd(const std::string& tenantId) { return "tenant_keys|" + tenantId; }

Dek unwrap(const std::string& tenantId, const std::string& kekId, const std::string& wrappedHex) {
  auto& s = state();
  if (kekId != s.kekId) {
    throw std::runtime_error("the data key of tenant " + tenantId + " was wrapped by another KEK (kek_id " + kekId +
                             ", this deployment has " + s.kekId + "); start with the KEK the data was written with");
  }
  return Dek{aeadOpen(s.kek, wrapAd(tenantId), fromPgHex(wrappedHex))};
}

constexpr const char* kSelectKey =
    "SELECT kek_id, '\\x' || encode(wrapped, 'hex') AS wrapped FROM tenant_keys WHERE tenant_id = $1::uuid";

// In its own committed transaction, so a rolled-back caller never leaves a DEK cached that the database lacks.
Task<Dek> loadOrCreate(const std::string& tenantId) {
  auto tx = co_await db::TenantTx::beginSystem(tenantId);
  auto rows = co_await tx.exec(kSelectKey, tenantId);
  if (rows.empty()) {
    std::vector<unsigned char> dek(kKey);
    randombytes_buf(dek.data(), dek.size());
    const auto wrapped = aeadSeal(state().kek, wrapAd(tenantId), dek.data(), dek.size());
    sodium_memzero(dek.data(), dek.size());
    co_await tx.exec(
        "INSERT INTO tenant_keys (tenant_id, kek_id, wrapped) VALUES ($1::uuid, $2, $3::bytea) "
        "ON CONFLICT (tenant_id) DO NOTHING",
        tenantId, state().kekId, crypto::pgByteaHex(wrapped));
    rows = co_await tx.exec(kSelectKey, tenantId);
  }
  co_await tx.commit();
  co_return unwrap(tenantId, rows[0]["kek_id"].as<std::string>(), rows[0]["wrapped"].as<std::string>());
}

const std::map<std::string, std::set<std::string>> kColumns{
    {"goals", {"description"}},
    {"plans", {"body"}},
    {"work_roles", {"description"}},
    {"work_responsibilities", {"description"}},
    {"work_projects", {"description"}},
    {"work_deliverables", {"description"}},
    {"skill_sessions", {"notes"}},
    {"entertainment_slots", {"notes"}},
    {"tasks", {"notes"}},
    {"calendar_events", {"notes"}},
    {"activities", {"notes"}},
    {"detections", {"summary", "evidence"}},
};

}  // namespace

void init() {
  crypto::ensureInit();
  std::string text;
  std::string from;
  if (const auto file = env::get("THELIFE_KEK_FILE"); !file.empty()) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("THELIFE_KEK_FILE: cannot read " + file);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    from = "THELIFE_KEK_FILE";
  } else {
    text = env::get("THELIFE_KEK");
    from = "THELIFE_KEK";
    if (text.empty()) {
      throw std::runtime_error("no encryption key: set THELIFE_KEK_FILE (or THELIFE_KEK) to 32 random bytes as base64 "
                               "(openssl rand -base64 32); keep it with the database backups, data written with it "
                               "cannot be read without it");
    }
  }
  auto kek = decodeKey(std::move(text), from);
  auto& s = state();
  std::lock_guard lock(s.mu);
  s.kekId = crypto::hex(crypto::sha256("thelife-kek-id|" + std::string(kek.begin(), kek.end()))).substr(0, 16);
  s.kek = std::move(kek);
  s.deks.clear();
  s.ready = true;
}

bool ready() { return state().ready; }

const std::map<std::string, std::set<std::string>>& columns() { return kColumns; }

const std::set<std::string>* columnsOf(const std::string& table) {
  const auto it = kColumns.find(table);
  return it == kColumns.end() ? nullptr : &it->second;
}

Task<Dek> dekFor(db::TenantTx& tx) {
  auto& s = state();
  if (!s.ready) throw std::logic_error("fieldcrypt::init() was not called");
  const std::string tenant = tx.tenantId();
  {
    std::lock_guard lock(s.mu);
    if (auto it = s.deks.find(tenant); it != s.deks.end()) co_return it->second;
  }
  Dek dek = co_await loadOrCreate(tenant);
  std::lock_guard lock(s.mu);
  co_return s.deks.emplace(tenant, std::move(dek)).first->second;
}

std::vector<unsigned char> seal(const Dek& dek, const std::string& tenantId, const std::string& table,
                                const std::string& column, const Json::Value& value) {
  const auto text = json::compact(value);
  return aeadSeal(dek.key, associated(tenantId, table, column), reinterpret_cast<const unsigned char*>(text.data()),
                  text.size());
}

Json::Value open(const Dek& dek, const std::string& tenantId, const std::string& table, const std::string& column,
                 const std::vector<unsigned char>& bytes) {
  if (bytes.empty()) throw std::runtime_error("empty encrypted value");
  if (bytes[0] == kLegacy) return json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()) + 1,
                                                                bytes.size() - 1));
  const auto plain = aeadOpen(dek.key, associated(tenantId, table, column), bytes);
  return json::parse(std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size()));
}

std::vector<unsigned char> fromPgHex(const std::string& text) {
  if (text.size() < 2 || text[0] != '\\' || text[1] != 'x' || text.size() % 2 != 0) {
    throw std::runtime_error("not a bytea hex value");
  }
  std::vector<unsigned char> out((text.size() - 2) / 2);
  size_t len = 0;
  if (sodium_hex2bin(out.data(), out.size(), text.data() + 2, text.size() - 2, nullptr, &len, nullptr) != 0 ||
      len != out.size()) {
    throw std::runtime_error("not a bytea hex value");
  }
  return out;
}

Task<void> openRow(db::TenantTx& tx, const std::string& table, Json::Value& row) {
  const auto* cols = columnsOf(table);
  if (!cols || !row.isObject()) co_return;
  std::optional<Dek> dek;
  for (const auto& c : *cols) {
    if (!row.isMember(c) || !row[c].isString()) continue;
    if (!dek) dek = co_await dekFor(tx);
    row[c] = open(*dek, tx.tenantId(), table, c, fromPgHex(row[c].asString()));
  }
}

Task<void> sealFields(db::TenantTx& tx, const std::string& table, Json::Value& fields) {
  const auto* cols = columnsOf(table);
  if (!cols) co_return;
  std::optional<Dek> dek;
  for (const auto& c : *cols) {
    if (!fields.isMember(c) || fields[c].isNull()) continue;
    if (!dek) dek = co_await dekFor(tx);
    fields[c] = crypto::pgByteaHex(seal(*dek, tx.tenantId(), table, c, fields[c]));
  }
}

Task<Json::Value> openText(db::TenantTx& tx, const std::string& table, const std::string& column,
                           const std::string& pgHex) {
  if (pgHex.empty()) co_return Json::Value();
  const Dek dek = co_await dekFor(tx);
  co_return open(dek, tx.tenantId(), table, column, fromPgHex(pgHex));
}

Task<int> resealLegacy(const std::string& tenantId) {
  int sealed = 0;
  for (const auto& [table, cols] : kColumns) {
    for (const auto& col : cols) {
      for (;;) {
        auto tx = co_await db::TenantTx::beginSystem(tenantId);
        const Dek dek = co_await dekFor(tx);
        const auto rows = co_await tx.exec("SELECT id::text AS id, '\\x' || encode(\"" + col + "\", 'hex') AS v FROM \"" +
                                           table + "\" WHERE get_byte(\"" + col + "\", 0) = 0 LIMIT 200");
        for (const auto& r : rows) {
          const auto id = r["id"].as<std::string>();
          const auto old = r["v"].as<std::string>();
          const auto bytes = fromPgHex(old);
          const auto value = open(dek, tenantId, table, col, bytes);
          const auto hex = crypto::pgByteaHex(seal(dek, tenantId, table, col, value));
          if (table == "detections") {
            const std::vector<std::optional<std::string>> p{
                id, col == "summary" ? std::optional(hex) : std::nullopt,
                col == "evidence" ? std::optional(hex) : std::nullopt};
            co_await tx.execParams("SELECT app.detection_reseal($1::uuid, $2::bytea, $3::bytea)", p);
          } else {
            co_await tx.exec("UPDATE \"" + table + "\" SET \"" + col + "\" = $2::bytea WHERE id = $1::uuid AND \"" +
                                 col + "\" = $3::bytea",
                             id, hex, old);
          }
          ++sealed;
        }
        co_await tx.commit();
        if (rows.size() < 200) break;
      }
    }
  }
  co_return sealed;
}

void registerJobs() {
  jobs::registerHandler("fieldcrypt.reseal", [](const jobs::Job& j) -> Task<void> {
    const int n = co_await resealLegacy(j.tenantId);
    if (n > 0) LOG_INFO << "fieldcrypt.reseal: sealed " << n << " legacy values of tenant " << j.tenantId;
  });
}

void clearCache() {
  std::lock_guard lock(state().mu);
  state().deks.clear();
}

}  // namespace thelife::fieldcrypt
