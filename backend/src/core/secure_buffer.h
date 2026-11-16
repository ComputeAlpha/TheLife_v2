#pragma once

#include <memory>
#include <string>
#include <vector>

namespace thelife {

// Bytes that must never outlive their use (capture images, backend_lld.md §7.4): held in memory only and zeroed
// when destroyed, on every path. Move-only.
class SecureBuffer {
 public:
  SecureBuffer() = default;
  explicit SecureBuffer(std::string bytes) : bytes_(std::move(bytes)) {}
  ~SecureBuffer() { wipe(); }
  SecureBuffer(SecureBuffer&& o) noexcept : bytes_(std::move(o.bytes_)) { o.bytes_.clear(); }
  SecureBuffer& operator=(SecureBuffer&& o) noexcept {
    if (this != &o) {
      wipe();
      bytes_ = std::move(o.bytes_);
      o.bytes_.clear();
    }
    return *this;
  }
  SecureBuffer(const SecureBuffer&) = delete;
  SecureBuffer& operator=(const SecureBuffer&) = delete;

  const std::string& bytes() const { return bytes_; }
  std::size_t size() const { return bytes_.size(); }
  void wipe();

 private:
  std::string bytes_;
};

using Attachments = std::vector<SecureBuffer>;

}  // namespace thelife
