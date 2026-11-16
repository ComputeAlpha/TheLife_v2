#include "net/uploader.h"

#include <cstdlib>
#include <set>

#include "core/json.h"
#include "core/log.h"

namespace thelife::collector {

UploadResult Uploader::uploadOnce(SysTime now) {
  UploadResult r;
  const auto rows = q_.peek(kBatch);
  if (rows.empty()) return r;
  Json::Value body;
  auto& arr = body["observations"] = Json::Value(Json::arrayValue);
  std::vector<int64_t> unreadable;
  for (const auto& row : rows) {
    auto payload = json::parseStrict(row.payload, 4 * 1024 * 1024);
    if (!payload || !payload->isObject()) {
      unreadable.push_back(row.seq);
      continue;
    }
    Json::Value o;
    o["client_seq"] = Json::Int64(row.seq);
    o["captured_at"] = row.capturedAt;
    o["kind"] = row.kind;
    o["payload"] = std::move(*payload);
    arr.append(std::move(o));
  }
  if (!unreadable.empty()) {
    // A row that cannot be read back would block the queue forever: drop it and say so.
    q_.ack(unreadable);
    Json::Value gap;
    gap["from_seq"] = Json::Int64(unreadable.front());
    gap["to_seq"] = Json::Int64(unreadable.back());
    gap["reason"] = "corrupt";
    q_.push("gap", now, gap);
    r.dropped += static_cast<int>(unreadable.size());
  }
  if (arr.empty()) return r;
  r.sent = static_cast<int>(arr.size());
  const auto first = arr[0]["client_seq"].asInt64(), last = arr[arr.size() - 1]["client_seq"].asInt64();

  HttpRequest req;
  req.method = "POST";
  req.path = "/ingest/observations";
  req.body = json::compact(body);
  req.headers.emplace_back("Idempotency-Key", prefix_ + ":" + std::to_string(first) + "-" + std::to_string(last));
  const auto res = http_.send(req);
  if (!res.reached()) {
    r.kind = UploadResult::Kind::Retry;
    r.error = "backend unreachable: " + res.error;
    return r;
  }
  if (res.status == 401) {
    r.kind = UploadResult::Kind::Unauthorized;
    r.error = "token revoked or expired";
    return r;
  }
  if (res.status == 200 || res.status == 201) {
    const auto doc = json::parseStrict(res.body);
    std::set<int64_t> sent;
    for (const auto& o : arr) sent.insert(o["client_seq"].asInt64());
    std::vector<int64_t> done;
    if (doc && doc->isObject()) {
      for (const char* k : {"accepted", "duplicates"}) {
        for (const auto& s : (*doc)[k]) {
          if (s.isIntegral() && sent.count(s.asInt64())) done.push_back(s.asInt64());
        }
      }
    }
    q_.ack(done);
    r.kind = UploadResult::Kind::Uploaded;
    r.acked = static_cast<int>(done.size());
    return r;
  }
  if (res.status == 422) {
    // The batch was refused as invalid. Drop the rows the problem points at (/observations/<i>/…), or the whole
    // batch when it names none, so one bad row cannot wedge the queue; a gap records the loss.
    std::set<int64_t> bad;
    if (const auto doc = json::parseStrict(res.body); doc && doc->isObject()) {
      for (const auto& e : (*doc)["errors"]) {
        const auto ptr = e["path"].isString() ? e["path"].asString() : "";
        if (ptr.starts_with("/observations/")) {
          const auto idx = std::strtoul(ptr.c_str() + 14, nullptr, 10);
          if (idx < arr.size()) bad.insert(arr[static_cast<Json::ArrayIndex>(idx)]["client_seq"].asInt64());
        }
      }
    }
    if (bad.empty()) {
      for (const auto& o : arr) bad.insert(o["client_seq"].asInt64());
    }
    std::vector<int64_t> drop(bad.begin(), bad.end());
    q_.ack(drop);
    Json::Value gap;
    gap["from_seq"] = Json::Int64(drop.front());
    gap["to_seq"] = Json::Int64(drop.back());
    gap["reason"] = "corrupt";
    q_.push("gap", now, gap);
    log::warn("upload: backend rejected {} observation(s) as invalid; dropped", drop.size());
    r.kind = UploadResult::Kind::Rejected;
    r.dropped += static_cast<int>(drop.size());
    return r;
  }
  r.kind = UploadResult::Kind::Retry;
  r.retryAfterS = res.retryAfterS();
  r.error = "upload failed: HTTP " + std::to_string(res.status);
  return r;
}

}  // namespace thelife::collector
