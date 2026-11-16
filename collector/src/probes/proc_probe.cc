#include "probes/proc_probe.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "core/json.h"
#include "probes/window_probe.h"

namespace thelife::collector {

namespace {

std::optional<std::string> slurp(const std::string& path, size_t max = 64 * 1024) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::string s(max, '\0');
  f.read(s.data(), static_cast<std::streamsize>(max));
  s.resize(static_cast<size_t>(f.gcount()));
  return s;
}

std::string basename(const std::string& p) {
  const auto slash = p.rfind('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

}  // namespace

Json::Value ProcEntry::toJson() const {
  Json::Value p;
  p["exe"] = json::utf8Truncate(exe, 4096);
  p["comm"] = json::utf8Truncate(comm, 64);
  auto& ps = p["pids"] = Json::Value(Json::arrayValue);
  for (size_t i = 0; i < pids.size() && i < 1000; ++i) ps.append(pids[i]);
  p["cpu_pct"] = cpuPct;
  p["rss_mb"] = rssMb;
  p["has_window"] = hasWindow;
  p["focused"] = focused;
  if (!args.empty()) {
    for (size_t i = 0; i < args.size() && i < 32; ++i) p["args"].append(json::utf8Truncate(args[i], 1024));
  }
  return p;
}

ProcProbe::ProcProbe(std::string procRoot, uid_t uid, std::vector<std::string> argsAllowlist)
    : root_(std::move(procRoot)), uid_(uid), allow_(argsAllowlist.begin(), argsAllowlist.end()) {
  clkTck_ = ::sysconf(_SC_CLK_TCK);
  if (clkTck_ <= 0) clkTck_ = 100;
}

std::map<int, ProcProbe::Sample> ProcProbe::scan() const {
  std::map<int, Sample> out;
  DIR* d = ::opendir(root_.c_str());
  if (!d) return out;
  while (const auto* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
    const int pid = std::atoi(name.c_str());
    const std::string dir = root_ + "/" + name;
    const auto status = slurp(dir + "/status");
    if (!status) continue;  // exited meanwhile
    long uid = -1, ppid = -1;
    double rssKb = 0;
    std::string comm;
    std::istringstream ls(*status);
    for (std::string line; std::getline(ls, line);) {
      if (line.starts_with("Name:")) comm = line.substr(line.find_first_not_of(" \t", 5));
      else if (line.starts_with("PPid:")) ppid = std::atol(line.c_str() + 5);
      else if (line.starts_with("Uid:")) uid = std::atol(line.c_str() + 4);
      else if (line.starts_with("VmRSS:")) rssKb = std::atof(line.c_str() + 6);
    }
    if (uid != static_cast<long>(uid_) || pid == 2 || ppid == 2) continue;
    const auto cmdline = slurp(dir + "/cmdline", 16 * 1024);
    if (!cmdline || cmdline->empty()) continue;  // kernel thread or zombie
    std::vector<std::string> argv;
    for (size_t i = 0; i < cmdline->size();) {
      const auto nul = cmdline->find('\0', i);
      argv.push_back(cmdline->substr(i, nul == std::string::npos ? std::string::npos : nul - i));
      if (nul == std::string::npos) break;
      i = nul + 1;
    }
    Sample s;
    s.comm = comm;
    s.rssMb = rssKb / 1024.0;
    s.exe = readExe(root_, pid).value_or(argv.empty() ? comm : argv[0]);
    if (const auto stat = slurp(dir + "/stat", 4096)) {
      const auto close = stat->rfind(')');
      if (close != std::string::npos) {
        std::istringstream fs(stat->substr(close + 2));
        std::vector<std::string> f;
        for (std::string tok; fs >> tok && f.size() < 14;) f.push_back(tok);
        if (f.size() >= 13) s.ticks = std::strtoull(f[11].c_str(), nullptr, 10) + std::strtoull(f[12].c_str(), nullptr, 10);
      }
    }
    if (allow_.count(basename(s.exe)) && argv.size() > 1) s.args.assign(argv.begin() + 1, argv.end());
    out[pid] = std::move(s);
  }
  ::closedir(d);
  return out;
}

bool ProcProbe::changed(const ProcEntry& a, const ProcEntry& b) {
  const double rssDelta = std::abs(a.rssMb - b.rssMb);
  return std::abs(a.cpuPct - b.cpuPct) >= 5.0 || (rssDelta >= 10.0 && rssDelta >= 0.2 * std::max(a.rssMb, 1.0)) ||
         a.pids.size() != b.pids.size() || a.hasWindow != b.hasWindow || a.focused != b.focused;
}

Json::Value ProcProbe::snapshot(SysTime now, std::optional<int> focusedPid,
                                               const std::vector<int>& windowPids) {
  const auto samples = scan();
  const double dt = lastScan_ ? std::chrono::duration<double>(now - *lastScan_).count() : 0.0;
  const std::set<int> withWindow(windowPids.begin(), windowPids.end());
  std::map<std::string, ProcEntry> agg;
  std::map<int, unsigned long long> ticks;
  for (const auto& [pid, s] : samples) {
    auto& e = agg[s.exe];
    if (e.exe.empty()) {
      e.exe = s.exe;
      e.comm = s.comm;
      e.args = s.args;
    }
    e.pids.push_back(pid);
    e.rssMb += s.rssMb;
    if (const auto it = lastTicks_.find(pid); it != lastTicks_.end() && dt > 0 && s.ticks >= it->second) {
      e.cpuPct += static_cast<double>(s.ticks - it->second) / static_cast<double>(clkTck_) / dt * 100.0;
    }
    e.hasWindow = e.hasWindow || withWindow.count(pid);
    e.focused = e.focused || (focusedPid && *focusedPid == pid);
    ticks[pid] = s.ticks;
  }
  for (auto& [_, e] : agg) {
    e.cpuPct = round1(e.cpuPct);
    e.rssMb = round1(e.rssMb);
  }
  lastTicks_ = std::move(ticks);
  lastScan_ = now;
  current_ = agg;

  auto busiest = [](std::vector<const ProcEntry*> v) {
    std::sort(v.begin(), v.end(), [](const ProcEntry* a, const ProcEntry* b) {
      return a->cpuPct != b->cpuPct ? a->cpuPct > b->cpuPct : a->rssMb > b->rssMb;
    });
    if (v.size() > kMaxEntries) v.resize(kMaxEntries);
    return v;
  };

  Json::Value p;
  if (!lastFull_ || now - *lastFull_ >= kFullEvery) {
    p["full"] = true;
    std::vector<const ProcEntry*> all;
    for (const auto& [_, e] : agg) all.push_back(&e);
    auto& procs = p["procs"] = Json::Value(Json::arrayValue);
    for (const auto* e : busiest(all)) procs.append(e->toJson());
    lastFull_ = now;
    sent_ = std::move(agg);
    return p;
  }
  p["full"] = false;
  std::vector<const ProcEntry*> started, changedV;
  std::vector<std::string> exited;
  for (const auto& [exe, e] : agg) {
    const auto it = sent_.find(exe);
    if (it == sent_.end()) started.push_back(&e);
    else if (changed(e, it->second)) changedV.push_back(&e);
  }
  for (const auto& [exe, _] : sent_) {
    if (!agg.count(exe)) exited.push_back(exe);
  }
  // An empty delta is still sent: it tells the backend the collector is alive (its focus spans end after five
  // minutes of silence).
  if (started.empty() && changedV.empty() && exited.empty()) return p;
  auto& st = p["started"] = Json::Value(Json::arrayValue);
  for (const auto* e : busiest(started)) {
    st.append(e->toJson());
    sent_[e->exe] = *e;
  }
  auto& ch = p["changed"] = Json::Value(Json::arrayValue);
  for (const auto* e : busiest(changedV)) {
    ch.append(e->toJson());
    sent_[e->exe] = *e;
  }
  auto& ex = p["exited"] = Json::Value(Json::arrayValue);
  for (size_t i = 0; i < exited.size() && i < kMaxEntries; ++i) {
    ex.append(json::utf8Truncate(exited[i], 4096));
    sent_.erase(exited[i]);
  }
  return p;
}

std::vector<ProcEntry> ProcProbe::top(size_t n) const {
  std::vector<ProcEntry> v;
  for (const auto& [_, e] : current_) v.push_back(e);
  std::sort(v.begin(), v.end(), [](const ProcEntry& a, const ProcEntry& b) {
    return a.cpuPct != b.cpuPct ? a.cpuPct > b.cpuPct : a.rssMb > b.rssMb;
  });
  if (v.size() > n) v.resize(n);
  return v;
}

}  // namespace thelife::collector
