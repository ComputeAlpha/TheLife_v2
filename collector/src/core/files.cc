#include "core/files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace thelife::collector::files {

namespace {

std::string homeDir() {
  const char* h = std::getenv("HOME");
  return h && *h ? h : "/";
}

std::string xdg(const char* override, const char* xdgVar, const char* fallback) {
  if (const char* o = std::getenv(override); o && *o) return o;
  if (const char* x = std::getenv(xdgVar); x && *x && x[0] == '/') return std::string(x) + "/thelife-collector";
  return homeDir() + fallback + "/thelife-collector";
}

std::string sysErr(const std::string& what) { return what + ": " + std::strerror(errno); }

}  // namespace

std::string configDir() { return xdg("THELIFE_COLLECTOR_CONFIG_DIR", "XDG_CONFIG_HOME", "/.config"); }
std::string stateDir() { return xdg("THELIFE_COLLECTOR_STATE_DIR", "XDG_STATE_HOME", "/.local/state"); }

bool ensurePrivateDir(const std::string& dir, std::string* err) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    if (err) *err = dir + ": " + ec.message();
    return false;
  }
  if (::chmod(dir.c_str(), 0700) != 0) {
    if (err) *err = sysErr(dir);
    return false;
  }
  return true;
}

std::optional<std::string> readPrivate(const std::string& path, std::string* err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (err) *err = sysErr(path);
    return std::nullopt;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    if (err) *err = path + ": not a regular file";
    return std::nullopt;
  }
  if (st.st_uid != ::getuid() || (st.st_mode & 077) != 0) {
    ::close(fd);
    if (err) *err = path + ": must be owned by you with mode 0600 (run: chmod 600 " + path + ")";
    return std::nullopt;
  }
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      ::close(fd);
      if (err) *err = sysErr(path);
      return std::nullopt;
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > 1024 * 1024) {
      ::close(fd);
      if (err) *err = path + ": too large";
      return std::nullopt;
    }
  }
  ::close(fd);
  return out;
}

bool writePrivateAtomic(const std::string& path, const std::string& content, std::string* err) {
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    if (err) *err = sysErr(tmp);
    return false;
  }
  size_t off = 0;
  while (off < content.size()) {
    const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      if (err) *err = sysErr(tmp);
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    if (err) *err = sysErr(tmp);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = sysErr(path);
    ::unlink(tmp.c_str());
    return false;
  }
  const auto dir = std::filesystem::path(path).parent_path();
  if (const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

}  // namespace thelife::collector::files
