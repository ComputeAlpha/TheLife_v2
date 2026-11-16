#include "queue/queue.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <stdexcept>

#include "core/json.h"

namespace thelife::collector {

namespace {

struct Stmt {
  sqlite3_stmt* s = nullptr;
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("queue: ") + sqlite3_errmsg(db));
    }
  }
  ~Stmt() { sqlite3_finalize(s); }
  Stmt& bind(int i, int64_t v) {
    sqlite3_bind_int64(s, i, v);
    return *this;
  }
  Stmt& bind(int i, std::string_view v) {
    sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  bool step() {
    const int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("queue: ") + sqlite3_errmsg(sqlite3_db_handle(s)));
  }
  int64_t i64(int c) { return sqlite3_column_int64(s, c); }
  std::string text(int c) {
    const auto* p = sqlite3_column_text(s, c);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(sqlite3_column_bytes(s, c))) : "";
  }
};

int64_t unixSeconds(SysTime t) {
  return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

}  // namespace

Queue::Queue(const std::string& path) {
  const mode_t old = ::umask(077);
  const int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                 nullptr);
  ::umask(old);
  if (rc != SQLITE_OK) {
    const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("queue: " + path + ": " + msg);
  }
  ::chmod(path.c_str(), 0600);
  sqlite3_busy_timeout(db_, 5000);
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=NORMAL");
  exec("PRAGMA temp_store=MEMORY");
  exec("PRAGMA secure_delete=ON");  // acknowledged rows (window titles) are overwritten, not just unlinked
  exec("BEGIN IMMEDIATE");
  try {
    exec(
        "CREATE TABLE IF NOT EXISTS obs (seq INTEGER PRIMARY KEY AUTOINCREMENT, captured_at TEXT NOT NULL, "
        "captured_unix INTEGER NOT NULL, kind TEXT NOT NULL, payload TEXT NOT NULL)");
    exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    Stmt has(db_, "SELECT count(*) FROM sqlite_sequence WHERE name = 'obs'");
    has.step();
    if (has.i64(0) == 0) {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      Stmt(db_, "INSERT INTO sqlite_sequence (name, seq) VALUES ('obs', ?)").bind(1, micros).step();
    }
    exec("COMMIT");
  } catch (...) {
    exec("ROLLBACK");
    throw;
  }
}

Queue::~Queue() { sqlite3_close(db_); }

void Queue::exec(const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown error";
    sqlite3_free(err);
    throw std::runtime_error("queue: " + msg);
  }
}

int64_t Queue::pushLocked(std::string_view kind, SysTime at, const std::string& payload) {
  Stmt s(db_, "INSERT INTO obs (captured_at, captured_unix, kind, payload) VALUES (?, ?, ?, ?)");
  s.bind(1, rfc3339(at)).bind(2, unixSeconds(at)).bind(3, kind).bind(4, payload).step();
  return sqlite3_last_insert_rowid(db_);
}

int64_t Queue::push(std::string_view kind, SysTime at, const Json::Value& payload) {
  const auto text = json::compact(payload);
  std::lock_guard lock(mu_);
  return pushLocked(kind, at, text);
}

std::vector<QueuedObservation> Queue::peek(int max) {
  std::lock_guard lock(mu_);
  std::vector<QueuedObservation> out;
  Stmt s(db_, "SELECT seq, captured_at, kind, payload FROM obs ORDER BY seq LIMIT ?");
  s.bind(1, static_cast<int64_t>(max));
  while (s.step()) out.push_back({s.i64(0), s.text(1), s.text(2), s.text(3)});
  return out;
}

void Queue::ack(const std::vector<int64_t>& seqs) {
  if (seqs.empty()) return;
  std::lock_guard lock(mu_);
  exec("BEGIN IMMEDIATE");
  try {
    Stmt s(db_, "DELETE FROM obs WHERE seq = ?");
    for (const auto seq : seqs) {
      sqlite3_reset(s.s);
      s.bind(1, seq).step();
    }
    exec("COMMIT");
  } catch (...) {
    exec("ROLLBACK");
    throw;
  }
}

int64_t Queue::depth() {
  std::lock_guard lock(mu_);
  Stmt s(db_, "SELECT count(*) FROM obs");
  s.step();
  return s.i64(0);
}

int64_t Queue::dropRange(const char* where, int64_t arg, const char* reason, SysTime now) {
  const std::string range = std::string("SELECT min(seq), max(seq), count(*) FROM obs WHERE ") + where;
  Stmt r(db_, range.c_str());
  r.bind(1, arg);
  r.step();
  const int64_t n = r.i64(2);
  if (n == 0) return 0;
  const int64_t from = r.i64(0), to = r.i64(1);
  const std::string del = std::string("DELETE FROM obs WHERE ") + where;
  Stmt(db_, del.c_str()).bind(1, arg).step();
  Json::Value gap;
  gap["from_seq"] = Json::Int64(from);
  gap["to_seq"] = Json::Int64(to);
  gap["reason"] = reason;
  pushLocked("gap", now, json::compact(gap));
  return n;
}

int64_t Queue::enforceCap(SysTime now, Seconds maxAge, int64_t maxRows) {
  std::lock_guard lock(mu_);
  exec("BEGIN IMMEDIATE");
  try {
    int64_t dropped = dropRange("captured_unix < ? AND kind <> 'gap'", unixSeconds(now - maxAge), "queue_expired", now);
    Stmt c(db_, "SELECT count(*) FROM obs");
    c.step();
    const int64_t excess = c.i64(0) - maxRows;
    if (excess > 0) {
      // The oldest `excess + 1` rows go (one more to make room for the gap row itself).
      Stmt cut(db_, "SELECT seq FROM obs ORDER BY seq LIMIT 1 OFFSET ?");
      cut.bind(1, excess);
      if (cut.step()) dropped += dropRange("seq <= ?", cut.i64(0), "queue_full", now);
    }
    exec("COMMIT");
    return dropped;
  } catch (...) {
    exec("ROLLBACK");
    throw;
  }
}

std::optional<std::string> Queue::meta(std::string_view key) {
  std::lock_guard lock(mu_);
  Stmt s(db_, "SELECT value FROM meta WHERE key = ?");
  s.bind(1, key);
  if (!s.step()) return std::nullopt;
  return s.text(0);
}

void Queue::setMeta(std::string_view key, const std::string& value) {
  std::lock_guard lock(mu_);
  Stmt(db_, "INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT (key) DO UPDATE SET value = excluded.value")
      .bind(1, key)
      .bind(2, value)
      .step();
}

void Queue::deleteMeta(std::string_view key) {
  std::lock_guard lock(mu_);
  Stmt(db_, "DELETE FROM meta WHERE key = ?").bind(1, key).step();
}

}  // namespace thelife::collector
