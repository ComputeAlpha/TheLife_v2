#pragma once

#include <string>
#include <string_view>

namespace thelife::ids {

// RFC 9562 UUIDv7: 48-bit unix-ms timestamp + random bits. Time-ordered, so B-tree friendly and usable as a
// cursor tie-breaker. Canonical lowercase 8-4-4-4-12 form.
std::string newUuidV7();

// Accepts canonical (any case) UUID text; does not check the version nibble.
bool isUuid(std::string_view s);

}  // namespace thelife::ids
