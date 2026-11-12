#include "core/json.h"

#include <json/reader.h>
#include <json/writer.h>

#include <memory>
#include <stdexcept>

namespace thelife::json {

Json::Value parse(std::string_view text) {
  Json::CharReaderBuilder b;
  b["collectComments"] = false;
  b["rejectDupKeys"] = true;
  b["stackLimit"] = 64;
  b["failIfExtra"] = true;
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  Json::Value out;
  std::string errs;
  if (!reader->parse(text.data(), text.data() + text.size(), &out, &errs)) {
    throw std::invalid_argument("malformed JSON");
  }
  return out;
}

std::string compact(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  b["emitUTF8"] = true;
  return Json::writeString(b, v);
}

}  // namespace thelife::json
