// libFuzzer target: GET /devices/self/notifications responses and what is made of them (collector_lld.md §7.5, §8).
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/json.h"
#include "notify/notifications.h"

using namespace thelife::collector;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  // Raw bytes as a title or body: escaped, capped, valid UTF-8, no markup left.
  const auto plain = plainText(std::string(text), 400);
  if (plain.find('<') != std::string::npos || plain.find('>') != std::string::npos) __builtin_trap();
  if (json::utf8Truncate(plain, plain.size()) != plain) __builtin_trap();

  const auto doc = json::parseStrict(text);
  if (!doc) return 0;
  NoteHolder holder;
  const SysTime now{};
  for (const auto& n : parseNotes(*doc)) {
    if (!isUuid(n.id)) __builtin_trap();
    if (const auto url = clickUrl("http://127.0.0.1:5000", n)) {
      // Only ever the local web UI, a fixed route and a UUID.
      static const std::string kApproval = "http://127.0.0.1:5000/chat#approval-";
      if (*url != "http://127.0.0.1:5000/chat" &&
          !(url->starts_with(kApproval) && isUuid(url->substr(kApproval.size())))) {
        __builtin_trap();
      }
    }
    holder.add(n, now);
  }
  holder.due(now, true);
  return 0;
}
