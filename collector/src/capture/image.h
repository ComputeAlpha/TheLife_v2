#pragma once

#include <optional>
#include <string>

#include "core/bytes.h"
#include "x11/xsource.h"

namespace thelife::collector {

// Area-average downscale so the long side is at most `maxLongSide` (collector_lld.md §5.4). Returns the input
// unchanged (moved) when it is already small enough.
Image downscale(Image src, int maxLongSide);

// JPEG in memory (libjpeg-turbo, 4:2:0). The output buffer is a SecureBytes sized for the worst case, so the
// encoder never reallocates and leaves no copy behind.
std::optional<SecureBytes> encodeJpeg(const Image& img, int quality, std::string* err);

}  // namespace thelife::collector
