#pragma once

#include <json/json.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/time.h"

struct sqlite3;

namespace thelife::collector {

struct QueuedObservation {
  int64_t seq = 0;
  std::string capturedAt;  // RFC 3339
  std::string kind;
  std::string payload;  // compact JSON
};

// The durable observation queue (collector_lld.md §7.1): SQLite in WAL mode, 0600, rows deleted only once the
// backend acknowledged their client_seq. Screenshots never go in here. Thread-safe; other processes (the `pause`
// and `status` sub-commands) open the same file.
//
// client_seq values are never reused: the table is AUTOINCREMENT and a new file starts at the current Unix time
// in microseconds, so a deleted queue cannot collide with sequences the backend has already stored.
class Queue {
 public:
  explicit Queue(const std::string& path);  // throws std::runtime_error
  ~Queue();
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  int64_t push(std::string_view kind, SysTime capturedAt, const Json::Value& payload);
  std::vector<QueuedObservation> peek(int max);
  void ack(const std::vector<int64_t>& seqs);
  int64_t depth();

  // Drops rows older than maxAge, then the oldest rows beyond maxRows, and records a `gap` observation for each
  // dropped range. Returns the number of rows dropped.
  int64_t enforceCap(SysTime now, Seconds maxAge, int64_t maxRows);

  std::optional<std::string> meta(std::string_view key);
  void setMeta(std::string_view key, const std::string& value);
  void deleteMeta(std::string_view key);

 private:
  void exec(const char* sql);
  int64_t pushLocked(std::string_view kind, SysTime capturedAt, const std::string& payload);
  int64_t dropRange(const char* where, int64_t arg, const char* reason, SysTime now);

  std::mutex mu_;
  sqlite3* db_ = nullptr;
};

}  // namespace thelife::collector
