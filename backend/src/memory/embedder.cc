#include "memory/embedder.h"

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <cstdio>

#include "core/blocking.h"
#include "core/env.h"
#include "core/json.h"
#include "llm/transport.h"

namespace thelife::memory {

namespace {
std::string socketPath;
std::atomic<long long> downUntil{0};  // steady-clock ms; no calls before this after a failure

trantor::ConcurrentTaskQueue& pool() {
  static trantor::ConcurrentTaskQueue p(2, "embed");
  return p;
}
long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void initEmbedder() {
  socketPath = env::get("THELIFE_EMBEDDER_SOCKET");
  if (socketPath.empty()) {
    LOG_INFO << "embedder: off (THELIFE_EMBEDDER_SOCKET unset); retrieval uses keyword search only";
  } else {
    LOG_INFO << "embedder: " << socketPath;
  }
}

bool embedderConfigured() { return !socketPath.empty(); }

std::string vectorLiteral(const std::vector<float>& v) {
  std::string s = "[";
  char buf[24];
  for (std::size_t i = 0; i < v.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%.6g", static_cast<double>(v[i]));
    if (i) s += ',';
    s += buf;
  }
  return s + "]";
}

drogon::Task<std::optional<Embeddings>> embed(std::vector<std::string> texts) {
  if (socketPath.empty() || texts.empty() || nowMs() < downUntil.load()) co_return std::nullopt;
  Embeddings out;
  for (std::size_t start = 0; start < texts.size(); start += 8) {
    Json::Value body;
    body["max_tokens"] = 512;
    for (std::size_t i = start; i < std::min(texts.size(), start + 8); ++i) body["texts"].append(texts[i]);
    const auto payload = json::compact(body);
    const auto path = socketPath;
    const auto r = co_await runBlockingOn(pool(), [path, payload] {
      const auto t = llm::makeCurlTransport(path);
      return t->post("http://embedder/v1/embed", {{"Content-Type", "application/json"}}, payload, 30, 30, {});
    });
    Json::Value v;
    bool ok = r.status == 200;
    if (ok) {
      try {
        v = json::parse(r.body);
        ok = v["vectors"].isArray() && v["vectors"].size() == body["texts"].size();
      } catch (const std::exception&) {
        ok = false;
      }
    }
    if (!ok) {
      LOG_WARN << "embedder unavailable (status " << r.status << (r.transportError.empty() ? "" : ", " + r.transportError)
               << "); keyword search only for 30 s";
      downUntil = nowMs() + 30000;
      co_return std::nullopt;
    }
    out.model = v["model"].asString();
    out.modelVersion = v["model_version"].asString();
    for (const auto& row : v["vectors"]) {
      std::vector<float> vec;
      vec.reserve(row.size());
      for (const auto& x : row) vec.push_back(x.asFloat());
      out.vectors.push_back(std::move(vec));
    }
  }
  co_return out;
}

}  // namespace thelife::memory
