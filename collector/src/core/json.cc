#include "core/json.h"

#include <memory>

namespace thelife::collector::json {

std::optional<Json::Value> parseStrict(std::string_view text, size_t maxBytes) {
  if (text.size() > maxBytes) return std::nullopt;
  Json::CharReaderBuilder b;
  Json::CharReaderBuilder::strictMode(&b.settings_);
  b.settings_["stackLimit"] = kMaxDepth;
  b.settings_["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> r(b.newCharReader());
  Json::Value v;
  std::string err;
  try {
    if (!r->parse(text.data(), text.data() + text.size(), &v, &err)) return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;  // stackLimit exceeded
  }
  return v;
}

std::string compact(const Json::Value& v) {
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  w["emitUTF8"] = true;
  return Json::writeString(w, v);
}

std::string utf8Truncate(std::string_view s, size_t maxBytes) {
  std::string out;
  out.reserve(std::min(s.size(), maxBytes));
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool valid = len > 0 && i + len <= s.size();
    for (size_t k = 1; valid && k < len; ++k) valid = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    if (!valid) {
      if (out.size() + 3 > maxBytes) break;
      out += "\xEF\xBF\xBD";  // U+FFFD
      ++i;
      continue;
    }
    if (c < 0x20 && c != '\t') {  // control characters carry nothing useful in a title
      if (out.size() + 1 > maxBytes) break;
      out += ' ';
      ++i;
      continue;
    }
    if (out.size() + len > maxBytes) break;
    out.append(s.substr(i, len));
    i += len;
  }
  return out;
}

}  // namespace thelife::collector::json
