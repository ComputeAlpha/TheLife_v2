#pragma once

#include <format>
#include <string>
#include <string_view>

// Logs go to stderr (journald under systemd). Never log the device token, window titles or capture bytes.
namespace thelife::collector::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };
void setLevel(Level l);
bool parseLevel(std::string_view s, Level& out);
void write(Level l, std::string_view msg);

template <typename... A>
void debug(std::format_string<A...> f, A&&... a) { write(Level::Debug, std::format(f, std::forward<A>(a)...)); }
template <typename... A>
void info(std::format_string<A...> f, A&&... a) { write(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <typename... A>
void warn(std::format_string<A...> f, A&&... a) { write(Level::Warn, std::format(f, std::forward<A>(a)...)); }
template <typename... A>
void error(std::format_string<A...> f, A&&... a) { write(Level::Error, std::format(f, std::forward<A>(a)...)); }

}  // namespace thelife::collector::log
