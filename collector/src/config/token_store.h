#pragma once

#include <mutex>
#include <optional>
#include <string>

namespace thelife::collector {

// The device token (collector_lld.md §4.1, §8): read from a 0600 file, held in memory, never logged and never put
// on a command line. Rotation writes the new token to disk before it is used.
class TokenStore {
 public:
  explicit TokenStore(std::string path) : path_(std::move(path)) {}

  bool load(std::string* err);
  std::string get() const;
  bool replace(const std::string& token, std::string* err);

  static bool wellFormed(const std::string& token);

 private:
  std::string path_;
  mutable std::mutex mu_;
  std::string token_;
};

}  // namespace thelife::collector
