#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "db/tenant_tx.h"

// Envelope encryption of selected free-text fields (backend_lld.md §9, migration 0014). Each tenant has a 32-byte
// data key (DEK) stored wrapped by the deployment key (KEK) in tenant_keys; values are sealed with
// XChaCha20-Poly1305 and "<tenant>|<table>.<column>" as associated data, so a value moved to another tenant or
// column fails to open. Protects database dumps, backups and direct database access, not a compromised app.
namespace thelife::fieldcrypt {

// Loads the KEK from THELIFE_KEK_FILE (a file holding the key) or THELIFE_KEK: 32 bytes as base64 (standard or
// URL-safe, padding optional) or 64 hex characters. Throws with a message naming the variable when missing or
// malformed. Call once at startup when a database is configured.
void init();
bool ready();

// The encrypted columns, table → columns (also the list the catalog test checks to be bytea).
const std::map<std::string, std::set<std::string>>& columns();
const std::set<std::string>* columnsOf(const std::string& table);

struct Dek {
  std::vector<unsigned char> key;  // 32 bytes
};

// The tenant's DEK, created (in its own committed transaction) on first use and cached in memory.
drogon::Task<Dek> dekFor(db::TenantTx& tx);

// Seals the JSON text of `value`. Returns the raw bytes; use crypto::pgByteaHex to pass them as a parameter.
std::vector<unsigned char> seal(const Dek& dek, const std::string& tenantId, const std::string& table,
                                const std::string& column, const Json::Value& value);
// Opens a sealed (0x01) or legacy (0x00) value. Throws std::runtime_error when it does not authenticate.
Json::Value open(const Dek& dek, const std::string& tenantId, const std::string& table, const std::string& column,
                 const std::vector<unsigned char>& bytes);

// bytea as Postgres prints it ("\x0a1b...", the form to_jsonb(row) and text results use) ↔ bytes.
std::vector<unsigned char> fromPgHex(const std::string& text);

// In a to_jsonb(row) object of `table`: replaces each encrypted column's "\x..." text by its opened value.
drogon::Task<void> openRow(db::TenantTx& tx, const std::string& table, Json::Value& row);
// In a field map about to be written to `table` (jsonb_populate_record input): replaces each non-null encrypted
// field by the "\x..." text of its sealed bytes.
drogon::Task<void> sealFields(db::TenantTx& tx, const std::string& table, Json::Value& fields);

// One bytea column value read as text ("\x..." or empty for NULL) → opened JSON (null when empty).
drogon::Task<Json::Value> openText(db::TenantTx& tx, const std::string& table, const std::string& column,
                                   const std::string& pgHex);

// Seals every legacy (0x00) value of the tenant. Returns how many values it sealed.
drogon::Task<int> resealLegacy(const std::string& tenantId);

// Registers the fieldcrypt.reseal job.
void registerJobs();

// Tests: forget cached DEKs (a test that swaps the KEK).
void clearCache();

}  // namespace thelife::fieldcrypt
