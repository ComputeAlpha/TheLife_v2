#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace thelife::collector {

// A fixed-size byte buffer that is zeroed when it is released (screen pixels and encoded captures,
// collector_lld.md §5.4). It never reallocates, so no copy of its contents is left behind in freed memory.
class SecureBytes {
 public:
  SecureBytes() = default;
  explicit SecureBytes(size_t capacity) : data_(new uint8_t[capacity]()), cap_(capacity), size_(capacity) {}
  ~SecureBytes() { wipe(); }
  SecureBytes(const SecureBytes&) = delete;
  SecureBytes& operator=(const SecureBytes&) = delete;
  SecureBytes(SecureBytes&& o) noexcept : data_(std::move(o.data_)), cap_(o.cap_), size_(o.size_) {
    o.cap_ = o.size_ = 0;
  }
  SecureBytes& operator=(SecureBytes&& o) noexcept {
    if (this != &o) {
      wipe();
      data_ = std::move(o.data_);
      cap_ = o.cap_;
      size_ = o.size_;
      o.cap_ = o.size_ = 0;
    }
    return *this;
  }

  uint8_t* data() { return data_.get(); }
  const uint8_t* data() const { return data_.get(); }
  size_t size() const { return size_; }
  size_t capacity() const { return cap_; }
  bool empty() const { return size_ == 0; }
  // Shrinks the visible size (for example to the encoded JPEG length); the capacity stays allocated.
  void setSize(size_t n) { size_ = n < cap_ ? n : cap_; }

  void wipe() {
    if (data_) {
      explicit_bzero(data_.get(), cap_);
      data_.reset();
    }
    cap_ = size_ = 0;
  }

 private:
  std::unique_ptr<uint8_t[]> data_;
  size_t cap_ = 0, size_ = 0;
};

}  // namespace thelife::collector
