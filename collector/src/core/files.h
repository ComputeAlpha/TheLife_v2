#pragma once

#include <optional>
#include <string>

namespace thelife::collector::files {

// ~/.config/thelife-collector and ~/.local/state/thelife-collector (collector_lld.md §4.1), following XDG.
// THELIFE_COLLECTOR_CONFIG_DIR / THELIFE_COLLECTOR_STATE_DIR override them (tests).
std::string configDir();
std::string stateDir();

// Creates the directory and its missing parents; the leaf gets mode 0700.
bool ensurePrivateDir(const std::string& dir, std::string* err);
// Reads a file that must be owned by the current user and have no group or other permission bits.
std::optional<std::string> readPrivate(const std::string& path, std::string* err);
// Writes through a 0600 temporary file, fsync and rename, so readers see the old or the new content.
bool writePrivateAtomic(const std::string& path, const std::string& content, std::string* err);

}  // namespace thelife::collector::files
