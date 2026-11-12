#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace thelife::crypto {

void ensureInit();  // idempotent sodium_init(); throws if libsodium cannot initialise

std::string base64Url(const unsigned char* data, size_t len);  // no padding
std::vector<unsigned char> sha256(std::string_view data);
// "\\x<hex>" literal for passing bytea as a text parameter.
std::string pgByteaHex(const std::vector<unsigned char>& bytes);

enum class TokenKind { UserSession, Device, ApiKey };

struct IssuedToken {
  std::string token;                // shown to the client once
  std::string prefix;               // first 8 chars, stored for identification
  std::vector<unsigned char> hash;  // sha256(token), the only thing stored
};

// "tlu_" / "tld_" / "tlk_" + 43 chars of base64url(32 random bytes).
IssuedToken newToken(TokenKind kind);
// Hash for lookup; returns empty when the token is malformed (wrong prefix or length), so callers can reject
// it without a database round trip.
std::vector<unsigned char> hashPresentedToken(std::string_view token);

// argon2id (libsodium crypto_pwhash_str, interactive limits). CPU and memory heavy: never call on an event loop
// thread, use thelife::runBlocking.
std::string hashPassword(std::string_view password);
bool verifyPassword(std::string_view password, const std::string& storedHash);
// Burns comparable time when the account does not exist, so response time does not reveal valid emails.
void dummyVerify(std::string_view password);

// Random password for admin-created accounts: 20 chars from an unambiguous alphabet.
std::string randomPassword();

}  // namespace thelife::crypto
