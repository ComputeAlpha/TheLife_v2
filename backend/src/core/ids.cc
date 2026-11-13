#include "core/ids.h"

#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "core/crypto.h"

namespace thelife::ids {

std::string newUuidV7() {
  crypto::ensureInit();
  std::array<uint8_t, 16> b{};
  auto ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  // Monotonic within the process (RFC 9562 §6.2 method 1): ids made in the same millisecond carry an increasing
  // 12-bit counter in rand_a, so ordering by id matches creation order (chat messages rely on it).
  uint16_t seq = 0;
  {
    static std::mutex m;
    static uint64_t lastMs = 0;
    static uint16_t counter = 0;
    std::lock_guard lock(m);
    if (ms <= lastMs) {
      ms = lastMs;
      if (++counter > 0x0FFF) {  // counter exhausted: borrow the next millisecond
        ++ms;
        counter = 0;
      }
    } else {
      counter = 0;
    }
    lastMs = ms;
    seq = counter;
  }
  for (int i = 0; i < 6; ++i) b[static_cast<size_t>(i)] = static_cast<uint8_t>(ms >> (40 - 8 * i));
  randombytes_buf(b.data() + 8, 8);
  b[6] = static_cast<uint8_t>(0x70 | ((seq >> 8) & 0x0F));  // version 7 + counter high bits
  b[7] = static_cast<uint8_t>(seq & 0xFF);
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
