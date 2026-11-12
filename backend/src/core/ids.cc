#include "core/ids.h"

#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdint>

#include "core/crypto.h"

namespace thelife::ids {

std::string newUuidV7() {
  crypto::ensureInit();
  std::array<uint8_t, 16> b{};
  const auto ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (int i = 0; i < 6; ++i) b[static_cast<size_t>(i)] = static_cast<uint8_t>(ms >> (40 - 8 * i));
  randombytes_buf(b.data() + 6, 10);
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x70);  // version 7
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[b[i] >> 4]);
    out.push_back(kHex[b[i] & 0x0F]);
  }
  return out;
}

bool isUuid(std::string_view s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

}  // namespace thelife::ids
