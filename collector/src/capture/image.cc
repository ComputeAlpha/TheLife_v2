#include "capture/image.h"

#include <turbojpeg.h>

#include <algorithm>
#include <vector>

namespace thelife::collector {

Image downscale(Image src, int maxLongSide) {
  const int longSide = std::max(src.width, src.height);
  if (longSide <= maxLongSide || maxLongSide <= 0) return src;
  const double scale = static_cast<double>(maxLongSide) / longSide;
  Image dst;
  dst.width = std::max(1, static_cast<int>(src.width * scale + 0.5));
  dst.height = std::max(1, static_cast<int>(src.height * scale + 0.5));
  dst.stride = dst.width * 4;
  dst.px = SecureBytes(static_cast<size_t>(dst.stride) * static_cast<size_t>(dst.height));
  // Each destination pixel averages the source box it covers (box filter; text stays legible for the model).
  std::vector<int> x0(static_cast<size_t>(dst.width) + 1);
  for (int x = 0; x <= dst.width; ++x) x0[static_cast<size_t>(x)] = std::min(src.width, static_cast<int>(static_cast<long long>(x) * src.width / dst.width));
  for (int y = 0; y < dst.height; ++y) {
    const int sy0 = static_cast<int>(static_cast<long long>(y) * src.height / dst.height);
    const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<long long>(y + 1) * src.height / dst.height));
    uint8_t* out = dst.px.data() + static_cast<size_t>(y) * static_cast<size_t>(dst.stride);
    for (int x = 0; x < dst.width; ++x) {
      const int sx0 = x0[static_cast<size_t>(x)], sx1 = std::max(sx0 + 1, x0[static_cast<size_t>(x) + 1]);
      unsigned long sum[3] = {0, 0, 0};
      for (int sy = sy0; sy < sy1; ++sy) {
        const uint8_t* row = src.px.data() + static_cast<size_t>(sy) * static_cast<size_t>(src.stride);
        for (int sx = sx0; sx < sx1; ++sx) {
          const uint8_t* p = row + static_cast<size_t>(sx) * 4;
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
        }
      }
      const unsigned long n = static_cast<unsigned long>(sy1 - sy0) * static_cast<unsigned long>(sx1 - sx0);
      out[x * 4 + 0] = static_cast<uint8_t>(sum[0] / n);
      out[x * 4 + 1] = static_cast<uint8_t>(sum[1] / n);
      out[x * 4 + 2] = static_cast<uint8_t>(sum[2] / n);
      out[x * 4 + 3] = 0;
    }
  }
  src.px.wipe();
  return dst;
}

std::optional<SecureBytes> encodeJpeg(const Image& img, int quality, std::string* err) {
  tjhandle h = tjInitCompress();
  if (!h) {
    if (err) *err = "turbojpeg init failed";
    return std::nullopt;
  }
  const unsigned long cap = tjBufSize(img.width, img.height, TJSAMP_420);
  SecureBytes out(cap);
  unsigned char* buf = out.data();
  unsigned long size = cap;
  const int rc = tjCompress2(h, img.px.data(), img.width, img.stride, img.height, TJPF_BGRX, &buf, &size, TJSAMP_420,
                             quality, TJFLAG_NOREALLOC | TJFLAG_FASTDCT);
  if (rc != 0 || buf != out.data()) {
    if (err) *err = std::string("JPEG encoding failed: ") + tjGetErrorStr2(h);
    tjDestroy(h);
    return std::nullopt;
  }
  tjDestroy(h);
  out.setSize(size);
  return out;
}

}  // namespace thelife::collector
