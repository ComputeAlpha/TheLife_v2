#include "llm/classifier.h"

#include <json/reader.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <memory>
#include <regex>
#include <sstream>

namespace thelife::llm {

const char* toString(Priority p) {
  switch (p) {
    case Priority::Interactive: return "interactive";
    case Priority::Proactive: return "proactive";
    case Priority::Vision: return "vision";
    case Priority::Background: return "background";
  }
  return "background";
}

const char* toString(ErrorClass c) {
  switch (c) {
    case ErrorClass::Transient: return "transient";
    case ErrorClass::RateLimited: return "rate_limited";
    case ErrorClass::Unclassified: return "unclassified";
    case ErrorClass::QuotaExhausted: return "quota_exhausted";
    case ErrorClass::ContextTooLong: return "context_too_long";
    case ErrorClass::AuthFailed: return "auth_failed";
    case ErrorClass::BadRequest: return "bad_request";
    case ErrorClass::InvalidOutput: return "invalid_output";
    case ErrorClass::ContentFiltered: return "content_filtered";
    case ErrorClass::ProviderUnavailable: return "provider_unavailable";
  }
  return "unclassified";
}

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Message, type and code of an OpenAI-style error body ({"error": {...}} or {"error": "text"}), or the raw body.
struct ErrorBody {
  std::string message, type, code;
  Json::Value json;
};

ErrorBody parseErrorBody(const std::string& body) {
  ErrorBody out;
  Json::CharReaderBuilder b;
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  std::string errs;
  Json::Value v;
  if (!body.empty() && reader->parse(body.data(), body.data() + body.size(), &v, &errs) && v.isObject()) {
    out.json = v;
    const auto& e = v["error"];
    if (e.isObject()) {
      if (e["message"].isString()) out.message = e["message"].asString();
      if (e["type"].isString()) out.type = e["type"].asString();
      if (e["code"].isString()) out.code = e["code"].asString();
      else if (e["code"].isIntegral()) out.code = std::to_string(e["code"].asInt64());
    } else if (e.isString()) {
      out.message = e.asString();
    } else if (v["message"].isString()) {
      out.message = v["message"].asString();
    }
  } else {
    out.message = body.substr(0, 500);
  }
  return out;
}

std::optional<int> headerInt(const HttpResult& r, const char* name) {
  const auto it = r.headers.find(name);
  if (it == r.headers.end()) return std::nullopt;
  try {
    std::size_t used = 0;
    const int v = std::stoi(it->second, &used);
    if (used == it->second.size() && v >= 0) return v;
  } catch (...) {
  }
  return std::nullopt;
}

}  // namespace

// RFC 3339 (UTC "Z" or offset) → time_point; nullopt if it does not parse.
std::optional<Clock::time_point> parseRfc3339(const std::string& s) {
  std::tm tm{};
  std::istringstream in(s);
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (in.fail()) return std::nullopt;
  std::string rest;
  std::getline(in, rest);
  if (!rest.empty() && rest[0] == '.') {
    const auto end = rest.find_first_not_of("0123456789", 1);
    rest = end == std::string::npos ? "" : rest.substr(end);
  }
  long offset = 0;
  if (rest == "Z" || rest == "z" || rest.empty()) {
    offset = 0;
  } else if ((rest[0] == '+' || rest[0] == '-') && rest.size() == 6 && rest[3] == ':') {
    offset = (std::stol(rest.substr(1, 2)) * 60 + std::stol(rest.substr(4, 2))) * 60;
    if (rest[0] == '-') offset = -offset;
  } else {
    return std::nullopt;
  }
  const auto t = timegm(&tm);
  if (t == -1) return std::nullopt;
  return Clock::from_time_t(t - offset);
}

std::string toRfc3339(Clock::time_point t) {
  const auto tt = Clock::to_time_t(t);
  std::tm tm{};
  gmtime_r(&tt, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

namespace {

const std::regex& quotaPattern() {
  static const std::regex re(
      R"(usage limit|weekly limit|hourly limit|daily limit|session limit|quota|limit (has been )?(reached|exceeded)|exceeded your|out of credits|insufficient credits|upgrade (your )?(plan|subscription))",
      std::regex::icase);
  return re;
}

std::string quotaScopeOf(const std::string& text) {
  const auto t = lower(text);
  if (t.find("week") != std::string::npos) return "weekly";
  if (t.find("hour") != std::string::npos) return "hourly";
  if (t.find("session") != std::string::npos) return "session";
  if (t.find("daily") != std::string::npos || t.find(" day") != std::string::npos) return "daily";
  return "unknown";
}

bool contextTooLong(const std::string& text) {
  static const std::regex re(
      R"(context (length|window)|maximum context|too many tokens|prompt is too long|exceeds? the (model'?s? )?(maximum|max)|input too long)",
      std::regex::icase);
  return std::regex_search(text, re);
}

}  // namespace

LlmError classify(const HttpResult& r, Clock::time_point now) {
  if (r.status == 0) {
    return LlmError(ErrorClass::Transient, "transport: " + (r.transportError.empty() ? "no response" : r.transportError));
  }
  const auto body = parseErrorBody(r.body);
  const std::string text = body.message + " " + body.type + " " + body.code;
  const auto retryAfter = headerInt(r, "retry-after");
  auto with = [&](LlmError e) {
    e.retryAfterSec = retryAfter;
    return e;
  };
  const std::string where = "HTTP " + std::to_string(r.status) + (body.type.empty() ? "" : " " + body.type);

  const bool quota = std::regex_search(text, quotaPattern()) || r.status == 402;
  if (quota && (r.status == 429 || r.status == 402 || r.status == 403)) {
    auto e = with(LlmError(ErrorClass::QuotaExhausted, where, r.status));
    e.quotaScope = quotaScopeOf(text);
    for (const char* key : {"resets_at", "reset_at", "reset_time"}) {
      const auto& v = body.json.isObject() && body.json["error"].isObject() ? body.json["error"][key] : body.json[key];
      if (v.isString()) {
        if (auto t = parseRfc3339(v.asString())) e.resetsAt = *t;
      }
    }
    if (!e.resetsAt && retryAfter && *retryAfter > 0) e.resetsAt = now + std::chrono::seconds(*retryAfter);
    return e;
  }
  if (r.status == 429 || lower(text).find("too many requests") != std::string::npos) {
    return with(LlmError(ErrorClass::RateLimited, where, r.status));
  }
  if (r.status == 401 || r.status == 403) return with(LlmError(ErrorClass::AuthFailed, where, r.status));
  if (r.status >= 500 || r.status == 408) return with(LlmError(ErrorClass::Transient, where, r.status));
  if ((r.status == 400 || r.status == 413) && contextTooLong(text)) {
    return with(LlmError(ErrorClass::ContextTooLong, where, r.status));
  }
  if (body.type == "content_filter" || lower(body.code) == "content_filter") {
    return with(LlmError(ErrorClass::ContentFiltered, where, r.status));
  }
  if (r.status == 400 || r.status == 404 || r.status == 405 || r.status == 413 || r.status == 422) {
    return with(LlmError(ErrorClass::BadRequest, where, r.status));
  }
  auto e = with(LlmError(ErrorClass::Unclassified, where, r.status));
  e.rawBody = r.body.substr(0, 64 * 1024);
  return e;
}

std::optional<LlmError> checkCompletion(const LlmResponse& r) {
  if (r.finishReason == "content_filter") return LlmError(ErrorClass::ContentFiltered, "finish_reason content_filter", 200);
  if (r.finishReason != "stop" && r.finishReason != "tool_calls" && r.finishReason != "length") {
    return LlmError(ErrorClass::InvalidOutput, "finish_reason '" + r.finishReason.substr(0, 32) + "'", 200);
  }
  if (r.content.empty() && r.toolCalls.empty()) {
    if (r.finishReason == "length") {
      // A reasoning model can spend the whole max_tokens budget thinking. Retrying the same request does the same.
      LlmError e(ErrorClass::InvalidOutput, "max_tokens reached while reasoning (no content, no tool calls)", 200);
      e.outOfTokens = true;
      return e;
    }
    return LlmError(ErrorClass::InvalidOutput, "empty content and no tool calls", 200);
  }
  return std::nullopt;
}

}  // namespace thelife::llm
