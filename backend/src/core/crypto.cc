#include "core/crypto.h"

#include <sodium.h>

#include <mutex>
#include <stdexcept>

namespace thelife::crypto {

void ensureInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialisation failed");
  });
}

std::string base64Url(const unsigned char* data, size_t len) {
  ensureInit();
  std::string out(sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(out.data(), out.size(), data, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::vector<unsigned char> sha256(std::string_view data) {
  ensureInit();
  std::vector<unsigned char> out(crypto_hash_sha256_BYTES);
  crypto_hash_sha256(out.data(), reinterpret_cast<const unsigned char*>(data.data()), data.size());
  return out;
}

std::string pgByteaHex(const std::vector<unsigned char>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex = "\\x";
  for (unsigned char b : bytes) {
    hex.push_back(kHex[b >> 4]);
    hex.push_back(kHex[b & 0x0F]);
  }
  return hex;
}

namespace {
constexpr size_t kTokenRandomBytes = 32;
constexpr size_t kTokenBodyLen = 43;  // base64url(32 bytes) without padding
constexpr size_t kTokenLen = 4 + kTokenBodyLen;

const char* prefixFor(TokenKind k) {
  switch (k) {
    case TokenKind::UserSession: return "tlu_";
    case TokenKind::Device: return "tld_";
    case TokenKind::ApiKey: return "tlk_";
  }
  return "tl?_";
}
}  // namespace

IssuedToken newToken(TokenKind kind) {
  ensureInit();
  unsigned char raw[kTokenRandomBytes];
  randombytes_buf(raw, sizeof raw);
  IssuedToken t;
  t.token = std::string(prefixFor(kind)) + base64Url(raw, sizeof raw);
  sodium_memzero(raw, sizeof raw);
  t.prefix = t.token.substr(0, 8);
  t.hash = sha256(t.token);
  return t;
}

std::vector<unsigned char> hashPresentedToken(std::string_view token) {
  if (token.size() != kTokenLen) return {};
  if (!(token.starts_with("tlu_") || token.starts_with("tld_") || token.starts_with("tlk_"))) return {};
  for (char c : token.substr(4)) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return {};
    }
  }
  return sha256(token);
}

std::string hashPassword(std::string_view password) {
  ensureInit();
  char out[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(out, password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    throw std::runtime_error("password hashing failed (out of memory)");
  }
  return out;
}

bool verifyPassword(std::string_view password, const std::string& storedHash) {
  ensureInit();
  return crypto_pwhash_str_verify(storedHash.c_str(), password.data(), password.size()) == 0;
}

void dummyVerify(std::string_view password) {
  // A fixed hash produced once with the same parameters, so the cost matches a real verification.
  static const std::string kDummy = hashPassword("thelife-dummy-password");
  (void)verifyPassword(password, kDummy);
}

std::string randomPassword() {
  ensureInit();
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  constexpr uint32_t n = sizeof(kAlphabet) - 1;
  std::string out;
  for (int i = 0; i < 20; ++i) out.push_back(kAlphabet[randombytes_uniform(n)]);
  return out;
}

}  // namespace thelife::crypto
