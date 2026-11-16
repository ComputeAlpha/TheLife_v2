#include "config/token_store.h"

#include <cstring>

#include "core/files.h"

namespace thelife::collector {

bool TokenStore::wellFormed(const std::string& t) {
  if (t.size() != 47 || !t.starts_with("tld_")) return false;
  for (size_t i = 4; i < t.size(); ++i) {
    const char c = t[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

bool TokenStore::load(std::string* err) {
  auto text = files::readPrivate(path_, err);
  if (!text) return false;
  while (!text->empty() && (text->back() == '\n' || text->back() == '\r' || text->back() == ' ')) text->pop_back();
  if (!wellFormed(*text)) {
    if (err) *err = path_ + ": not a device token (expected tld_…)";
    explicit_bzero(text->data(), text->size());
    return false;
  }
  std::lock_guard lock(mu_);
  token_ = *text;
  explicit_bzero(text->data(), text->size());
  return true;
}

std::string TokenStore::get() const {
  std::lock_guard lock(mu_);
  return token_;
}

bool TokenStore::replace(const std::string& token, std::string* err) {
  if (!wellFormed(token)) {
    if (err) *err = "the backend returned a malformed token";
    return false;
  }
  if (!files::writePrivateAtomic(path_, token + "\n", err)) return false;
  std::lock_guard lock(mu_);
  token_ = token;
  return true;
}

}  // namespace thelife::collector
