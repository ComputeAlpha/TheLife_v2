#include "core/time.h"

#include <cstdio>
#include <ctime>

namespace thelife::collector {

std::string rfc3339(SysTime t) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
  const std::time_t secs = static_cast<std::time_t>(ms / 1000 - (ms % 1000 < 0 ? 1 : 0));
  const int frac = static_cast<int>(((ms % 1000) + 1000) % 1000);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, frac);
  return buf;
}

std::optional<SysTime> parseRfc3339(std::string_view s) {
  int y, mo, d, h, mi, sec;
  int consumed = 0;
  const std::string str(s);
  if (std::sscanf(str.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &consumed) != 6 ||
      consumed != 19) {
    return std::nullopt;
  }
  size_t i = 19;
  long millis = 0;
  if (i < str.size() && str[i] == '.') {
    ++i;
    int digits = 0;
    while (i < str.size() && str[i] >= '0' && str[i] <= '9') {
      if (digits < 3) millis = millis * 10 + (str[i] - '0');
      ++digits;
      ++i;
    }
    if (digits == 0) return std::nullopt;
    for (; digits < 3; ++digits) millis *= 10;
  }
  long offset = 0;
  if (i < str.size() && (str[i] == 'Z' || str[i] == 'z')) {
    ++i;
  } else if (i + 6 == str.size() && (str[i] == '+' || str[i] == '-') && str[i + 3] == ':') {
    const int oh = std::atoi(str.substr(i + 1, 2).c_str()), om = std::atoi(str.substr(i + 4, 2).c_str());
    offset = (str[i] == '-' ? -1 : 1) * (oh * 3600L + om * 60L);
    i += 6;
  } else {
    return std::nullopt;
  }
  if (i != str.size() || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return std::nullopt;
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  const std::time_t t = timegm(&tm);
  return SysTime(std::chrono::seconds(t - offset)) + std::chrono::milliseconds(millis);
}

std::optional<Seconds> parseDuration(std::string_view s) {
  if (s.empty()) return std::nullopt;
  long total = 0, n = 0;
  bool haveDigits = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      n = n * 10 + (c - '0');
      if (n > 10'000'000) return std::nullopt;
      haveDigits = true;
      continue;
    }
    if (!haveDigits) return std::nullopt;
    switch (c) {
      case 's': total += n; break;
      case 'm': total += n * 60; break;
      case 'h': total += n * 3600; break;
      case 'd': total += n * 86400; break;
      default: return std::nullopt;
    }
    n = 0;
    haveDigits = false;
  }
  if (haveDigits || total <= 0 || total > 30L * 86400) return std::nullopt;
  return Seconds(total);
}

std::string formatDuration(Seconds d) {
  long s = d.count();
  std::string out;
  if (s >= 86400) out += std::to_string(s / 86400) + "d", s %= 86400;
  if (s >= 3600) out += std::to_string(s / 3600) + "h", s %= 3600;
  if (s >= 60) out += std::to_string(s / 60) + "m", s %= 60;
  if (s > 0 || out.empty()) out += std::to_string(s) + "s";
  return out;
}

}  // namespace thelife::collector
