#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace thelife::recurrence {

// A wall-clock time without zone ("floating" in RFC 5545). Rules are expanded in the user's local time and
// converted to instants by Postgres, so DST shifts keep a 09:00 meeting at 09:00.
struct LocalDateTime {
  int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;

  // "YYYY-MM-DD HH:MM:SS" (accepts 'T' as separator and ignores fractional seconds or offsets).
  static std::optional<LocalDateTime> parse(std::string_view text);
  std::string str() const;   // "YYYY-MM-DD HH:MM:SS"
  std::string date() const;  // "YYYY-MM-DD"
  LocalDateTime addSeconds(long long seconds) const;  // calendar arithmetic, no zone involved
  LocalDateTime addDays(int days) const { return addSeconds(86400LL * days); }
  LocalDateTime startOfDay() const { return LocalDateTime{year, month, day, 0, 0, 0}; }
  auto operator<=>(const LocalDateTime&) const = default;
};

// Returns an error message when `rule` is not an RRULE we accept (FREQ=DAILY|WEEKLY|MONTHLY|YEARLY, parsed by
// libical). An optional "RRULE:" prefix is allowed.
std::optional<std::string> validate(std::string_view rule);

// Starts of the occurrences in [from, to) of `rule` anchored at `dtstart`, oldest first, at most `limit`.
// Throws std::invalid_argument for a rule that does not validate.
std::vector<LocalDateTime> expand(std::string_view rule, const LocalDateTime& dtstart, const LocalDateTime& from,
                                  const LocalDateTime& to, size_t limit = 1000);

}  // namespace thelife::recurrence
