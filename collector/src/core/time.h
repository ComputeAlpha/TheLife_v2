#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace thelife::collector {

using SysTime = std::chrono::system_clock::time_point;
using Seconds = std::chrono::seconds;

// UTC with milliseconds, e.g. 2026-11-16T07:46:00.242Z.
std::string rfc3339(SysTime t);
// Accepts Z or a ±HH:MM offset and optional fractional seconds.
std::optional<SysTime> parseRfc3339(std::string_view s);
// "90s", "15m", "2h", "1h30m", "1d". Rejects zero, unknown units and anything above 30 days.
std::optional<Seconds> parseDuration(std::string_view s);
std::string formatDuration(Seconds d);

}  // namespace thelife::collector
