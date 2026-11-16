#include "core/secure_buffer.h"

#include <sodium.h>

namespace thelife {

void SecureBuffer::wipe() {
  if (!bytes_.empty()) sodium_memzero(bytes_.data(), bytes_.size());
  bytes_.clear();
  bytes_.shrink_to_fit();
}

}  // namespace thelife
