// RRULE validation and expansion (libical) in floating local time.
#include <gtest/gtest.h>

#include "core/recurrence.h"

using thelife::recurrence::expand;
using thelife::recurrence::LocalDateTime;
using thelife::recurrence::validate;

namespace {

LocalDateTime at(const char* s) { return *LocalDateTime::parse(s); }

std::vector<std::string> dates(const std::vector<LocalDateTime>& v) {
  std::vector<std::string> out;
  for (const auto& t : v) out.push_back(t.date());
  return out;
}

TEST(Recurrence, ValidatesRules) {
  EXPECT_FALSE(validate("FREQ=DAILY"));
  EXPECT_FALSE(validate("RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR"));
  EXPECT_FALSE(validate("FREQ=MONTHLY;BYDAY=-1FR;COUNT=3"));
  EXPECT_FALSE(validate("FREQ=YEARLY;UNTIL=20301231T000000Z"));
  EXPECT_TRUE(validate(""));
  EXPECT_TRUE(validate("FREQ=HOURLY"));        // too fine-grained for tasks and routines
  EXPECT_TRUE(validate("FREQ=SECONDLY"));
  EXPECT_TRUE(validate("FREQ=FORTNIGHTLY"));   // garbage must not abort the process (libical fatal errors off)
  EXPECT_TRUE(validate("freq=daily"));
  EXPECT_TRUE(validate("FREQ=DAILY;INTERVAL=0"));
  EXPECT_TRUE(validate("FREQ=DAILY\nX"));
  EXPECT_TRUE(validate(std::string(600, 'A')));
}

TEST(Recurrence, ExpandsWeeklyWithinHalfOpenWindow) {
  const auto out = expand("FREQ=WEEKLY;BYDAY=MO,WE", at("2026-11-02 07:00:00"), at("2026-11-09 00:00:00"),
                          at("2026-11-16 00:00:00"));
  EXPECT_EQ(dates(out), (std::vector<std::string>{"2026-11-09", "2026-11-11"}));
  EXPECT_EQ(out[0].hour, 7);
  // window end is exclusive
  EXPECT_EQ(expand("FREQ=DAILY", at("2026-11-01 00:00:00"), at("2026-11-01 00:00:00"),
                   at("2026-11-03 00:00:00")).size(),
            2u);
}

TEST(Recurrence, CountRunsFromDtstartNotFromWindow) {
  const auto out = expand("FREQ=DAILY;COUNT=5", at("2026-11-01 09:00:00"), at("2026-11-04 00:00:00"),
                          at("2026-12-01 00:00:00"));
  EXPECT_EQ(dates(out), (std::vector<std::string>{"2026-11-04", "2026-11-05"}));
}

TEST(Recurrence, LocalTimeIsKeptAcrossDstChanges) {
  // Floating time: 09:00 stays 09:00 on both sides of a DST switch; Postgres maps it to the instant.
  const auto out = expand("FREQ=DAILY", at("2026-10-24 09:00:00"), at("2026-10-24 00:00:00"),
                          at("2026-10-27 00:00:00"));
  ASSERT_EQ(out.size(), 3u);
  for (const auto& t : out) EXPECT_EQ(t.hour, 9);
}

TEST(Recurrence, LimitsAndInvalidRules) {
  EXPECT_EQ(expand("FREQ=DAILY", at("2000-01-01 00:00:00"), at("2000-01-01 00:00:00"), at("2100-01-01 00:00:00"), 10)
                .size(),
            10u);
  EXPECT_THROW(expand("FREQ=HOURLY", at("2026-01-01 00:00:00"), at("2026-01-01 00:00:00"),
                      at("2026-01-02 00:00:00")),
               std::invalid_argument);
  // a rule that can never occur is rejected up front
  EXPECT_EQ(validate("FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30").value_or(""), "can never occur");
}

TEST(Recurrence, LocalDateTimeArithmetic) {
  EXPECT_EQ(at("2028-02-28 23:30:00").addSeconds(3600).str(), "2028-02-29 00:30:00");  // leap year
  EXPECT_EQ(at("2026-12-31 00:00:00").addDays(1).date(), "2027-01-01");
  EXPECT_EQ(at("2026-03-01 00:00:00").addSeconds(-1).str(), "2026-02-28 23:59:59");
  EXPECT_EQ(at("2026-11-12").str(), "2026-11-12 00:00:00");
  EXPECT_EQ(at("2026-11-12T08:05:09.123+00:00").str(), "2026-11-12 08:05:09");
  EXPECT_FALSE(LocalDateTime::parse("2026-13-01"));
  EXPECT_FALSE(LocalDateTime::parse("yesterday"));
}

}  // namespace
