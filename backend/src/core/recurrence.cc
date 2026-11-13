#include "core/recurrence.h"

#include <libical/ical.h>

#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace thelife::recurrence {

namespace {

// libical aborts on errors by default; parsing user input must never do that.
void initLibical() {
  static std::once_flag once;
  std::call_once(once, [] { icalerror_set_errors_are_fatal(0); });
}

std::string stripPrefix(std::string_view rule) {
  constexpr std::string_view kPrefix = "RRULE:";
  if (rule.substr(0, kPrefix.size()) == kPrefix) rule.remove_prefix(kPrefix.size());
  return std::string(rule);
}

std::optional<icalrecurrencetype> parseRule(std::string_view rule, std::string* error) {
  initLibical();
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return std::nullopt;
  };
  const auto text = stripPrefix(rule);
  if (text.empty() || text.size() > 500) return fail("must be 1 to 500 characters");
  for (char c : text) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '=' || c == ';' || c == ',' ||
                    c == '-' || c == '+' || c == 'T' || c == 'Z';
    if (!ok) return fail("contains characters not allowed in an RRULE");
  }
  // On a parse error libical resets the rule (freq = NO_RECURRENCE). icalerrno is not used: it also carries
  // unrelated noise (the debug build sets it while loading built-in zone data for a UTC UNTIL).
  const icalrecurrencetype r = icalrecurrencetype_from_string(text.c_str());
  icalerror_clear_errno();
  if (r.freq == ICAL_NO_RECURRENCE) return fail("is not a valid RFC 5545 recurrence rule");
  if (r.freq != ICAL_DAILY_RECURRENCE && r.freq != ICAL_WEEKLY_RECURRENCE && r.freq != ICAL_MONTHLY_RECURRENCE &&
      r.freq != ICAL_YEARLY_RECURRENCE) {
    return fail("FREQ must be DAILY, WEEKLY, MONTHLY or YEARLY");
  }
  if (r.interval < 1 || r.interval > 1000) return fail("INTERVAL must be between 1 and 1000");
  // Rules libical cannot iterate (for example BYMONTHDAY=30 with BYMONTH=2) are rejected up front.
  icaltimetype probe = icaltime_null_time();
  probe.year = 2026;
  probe.month = 1;
  probe.day = 1;
  icalrecur_iterator* it = icalrecur_iterator_new(r, probe);
  icalerror_clear_errno();
  if (!it) return fail("can never occur");
  icalrecur_iterator_free(it);
  return r;
}

icaltimetype toIcal(const LocalDateTime& t) {
  icaltimetype v = icaltime_null_time();
  v.year = t.year;
  v.month = t.month;
  v.day = t.day;
  v.hour = t.hour;
  v.minute = t.minute;
  v.second = t.second;
  v.is_date = 0;
  v.zone = nullptr;  // floating
  return v;
}

LocalDateTime fromIcal(const icaltimetype& v) {
  return LocalDateTime{v.year, v.month, v.day, v.hour, v.minute, v.second};
}

}  // namespace

std::optional<LocalDateTime> LocalDateTime::parse(std::string_view text) {
  LocalDateTime t;
  if (text.size() < 10) return std::nullopt;
  const std::string s(text);
  char sep = ' ';
  int n = 0;
  if (s.size() >= 19) {
    n = std::sscanf(s.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d", &t.year, &t.month, &t.day, &sep, &t.hour, &t.minute,
                    &t.second);
    if (n != 7 || (sep != ' ' && sep != 'T')) return std::nullopt;
  } else {
    n = std::sscanf(s.c_str(), "%4d-%2d-%2d", &t.year, &t.month, &t.day);
    if (n != 3 || s.size() != 10) return std::nullopt;
  }
  if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 60) {
    return std::nullopt;
  }
  return t;
}

std::string LocalDateTime::str() const {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
  return buf;
}

std::string LocalDateTime::date() const { return str().substr(0, 10); }

namespace {
// Howard Hinnant's civil-from-days algorithms (proleptic Gregorian).
long long daysFromCivil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}
void civilFromDays(long long z, int& y, int& m, int& d) {
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(static_cast<long long>(yoe) + era * 400 + (m <= 2));
}
}  // namespace

LocalDateTime LocalDateTime::addSeconds(long long seconds) const {
  long long total = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL +
                    hour * 3600LL + minute * 60LL + second + seconds;
  long long days = total / 86400;
  long long rem = total % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  LocalDateTime t;
  civilFromDays(days, t.year, t.month, t.day);
  t.hour = static_cast<int>(rem / 3600);
  t.minute = static_cast<int>(rem % 3600 / 60);
  t.second = static_cast<int>(rem % 60);
  return t;
}

std::optional<std::string> validate(std::string_view rule) {
  std::string error;
  if (!parseRule(rule, &error)) return error;
  return std::nullopt;
}

std::vector<LocalDateTime> expand(std::string_view rule, const LocalDateTime& dtstart, const LocalDateTime& from,
                                  const LocalDateTime& to, size_t limit) {
  std::string error;
  const auto r = parseRule(rule, &error);
  if (!r) throw std::invalid_argument("rrule " + error);

  std::vector<LocalDateTime> out;
  if (!(from < to)) return out;
  const icaltimetype start = toIcal(dtstart);
  const icaltimetype lo = toIcal(from);
  const icaltimetype hi = toIcal(to);

  icalrecur_iterator* it = icalrecur_iterator_new(*r, start);
  icalerror_clear_errno();
  if (!it) return out;  // nothing can occur from this start
  // Skipping ahead is not allowed with COUNT (the count runs from dtstart); otherwise jump to the window.
  if (r->count == 0 && icaltime_compare(lo, start) > 0) icalrecur_iterator_set_start(it, lo);

  // Hard bound on iterations so a rule that rarely matches cannot spin.
  constexpr int kMaxSteps = 200000;
  for (int step = 0; step < kMaxSteps && out.size() < limit; ++step) {
    const icaltimetype t = icalrecur_iterator_next(it);
    if (icaltime_is_null_time(t) || icaltime_compare(t, hi) >= 0) break;
    if (icaltime_compare(t, lo) >= 0) out.push_back(fromIcal(t));
  }
  icalrecur_iterator_free(it);
  icalerror_clear_errno();
  return out;
}

}  // namespace thelife::recurrence
