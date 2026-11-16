#pragma once

#include <optional>
#include <string>

#include "net/http.h"
#include "queue/queue.h"

namespace thelife::collector {

// One batch of queued observations to POST /ingest/observations (collector_lld.md §7.1). Rows are deleted only for
// the client_seqs the backend lists as accepted or duplicate.
struct UploadResult {
  enum class Kind { Empty, Uploaded, Rejected, Retry, Unauthorized };
  Kind kind = Kind::Empty;
  int sent = 0, acked = 0, dropped = 0;
  std::optional<int> retryAfterS;
  std::string error;
};

class Uploader {
 public:
  static constexpr int kBatch = 500;
  // `keyPrefix` makes Idempotency-Keys unique per installation: "<prefix>:<first_seq>-<last_seq>".
  Uploader(Queue& q, IHttp& http, std::string keyPrefix) : q_(q), http_(http), prefix_(std::move(keyPrefix)) {}
  UploadResult uploadOnce(SysTime now);

 private:
  Queue& q_;
  IHttp& http_;
  std::string prefix_;
};

}  // namespace thelife::collector
