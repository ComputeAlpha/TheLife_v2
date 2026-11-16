#include "vision/captures.h"

#include <drogon/drogon.h>
#include <sodium.h>
#include <trantor/utils/Logger.h>

#include <deque>
#include <mutex>

#include "agent/prompt.h"
#include "core/crypto.h"
#include "core/env.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/field_crypt.h"
#include "db/tenant_tx.h"
#include "llm/gateway.h"

namespace thelife::vision {

namespace {

using drogon::Task;

struct Queue {
  std::mutex mu;
  std::deque<Capture> waiting;
  int inFlight = 0;
  bool running = false;
};

Queue& queue() {
  static Queue q;
  return q;
}

const Json::Value& analysisSchema() {
  static const Json::Value s = json::parse(R"({
    "type": "object", "additionalProperties": false,
    "required": ["area", "category", "summary", "confidence"],
    "properties": {
      "area": {"type": "string", "enum": ["work", "skill", "fitness", "personal", "entertainment", "other"]},
      "category": {"type": "string", "minLength": 1, "maxLength": 80},
      "summary": {"type": "string", "minLength": 1, "maxLength": 300},
      "confidence": {"type": "number", "minimum": 0, "maximum": 1},
      "apps": {"type": "array", "maxItems": 5, "items": {"type": "string", "maxLength": 80}}
    }})");
  return s;
}

std::string dataUri(const SecureBuffer& jpeg) {
  const auto& b = jpeg.bytes();
  std::string out(sodium_base64_ENCODED_LEN(b.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char*>(b.data()), b.size(),
                    sodium_base64_VARIANT_ORIGINAL);
  out.resize(out.size() - 1);  // the terminating NUL
  std::string uri = "data:image/jpeg;base64," + out;
  sodium_memzero(out.data(), out.size());
  return uri;
}

// What the model sees next to the images: the focused window, the busiest processes, what was detected in the
// last 30 minutes and what is planned now. Read in a short transaction (no transaction is held during the call).
Task<Json::Value> contextFor(const Capture& c, std::string& userName) {
  auto tx = co_await db::TenantTx::beginSystem(c.tenantId);
  const auto u = co_await tx.exec("SELECT display_name, timezone FROM users WHERE id = $1::uuid", c.userId);
  userName = u.empty() ? "the user" : u[0]["display_name"].as<std::string>();
  Json::Value v;
  v["captured_at"] = c.context["captured_at"];
  if (!u.empty()) v["timezone"] = u[0]["timezone"].as<std::string>();
  v["monitors"] = static_cast<Json::UInt>(c.images.size());
  v["focused_monitor"] = c.context["active_monitor"];
  if (c.context["window"].isObject()) {
    Json::Value w;
    for (const char* k : {"wm_class", "title", "exe", "fullscreen"}) {
      if (c.context["window"].isMember(k)) w[k] = c.context["window"][k];
    }
    v["focused_window"] = w;
  }
  if (c.context.isMember("idle_ms")) v["idle_ms"] = c.context["idle_ms"];
  // The busiest processes, by CPU, names only.
  std::vector<std::pair<double, std::string>> procs;
  for (const auto& p : c.context["processes"]) {
    if (p["exe"].isString()) procs.emplace_back(p.get("cpu_pct", 0.0).asDouble(), p["exe"].asString());
  }
  std::sort(procs.begin(), procs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  v["processes"] = Json::Value(Json::arrayValue);
  for (std::size_t i = 0; i < procs.size() && i < 15; ++i) {
    const auto slash = procs[i].second.rfind('/');
    v["processes"].append(slash == std::string::npos ? procs[i].second : procs[i].second.substr(slash + 1));
  }
  v["recent_detections"] = Json::Value(Json::arrayValue);
  for (const auto& r : co_await tx.exec(
           "SELECT source, area::text AS area, category, '\\x' || encode(summary, 'hex') AS s, "
           "  to_jsonb(started_at) #>> '{}' AS f, to_jsonb(ended_at) #>> '{}' AS t FROM detections "
           "WHERE ended_at > now() - interval '30 minutes' AND status IN ('detected', 'confirmed') "
           "ORDER BY started_at DESC LIMIT 6")) {
    Json::Value d;
    d["source"] = r["source"].as<std::string>();
    d["area"] = r["area"].as<std::string>();
    d["category"] = r["category"].as<std::string>();
    d["summary"] = co_await fieldcrypt::openText(tx, "detections", "summary", r["s"].as<std::string>());
    d["from"] = r["f"].as<std::string>();
    d["to"] = r["t"].as<std::string>();
    v["recent_detections"].append(d);
  }
  v["planned_now"] = Json::Value(Json::arrayValue);
  for (const auto& r : co_await tx.exec(
           "SELECT title, coalesce(area::text, '') AS area FROM calendar_events "
           "WHERE deleted_at IS NULL AND starts_at <= now() AND ends_at > now() ORDER BY starts_at LIMIT 5")) {
    Json::Value e;
    e["title"] = r["title"].as<std::string>();
    if (!r["area"].as<std::string>().empty()) e["area"] = r["area"].as<std::string>();
    v["planned_now"].append(e);
  }
  co_await tx.commit();
  co_return v;
}

void drain();

void finished() {
  auto& q = queue();
  std::lock_guard lock(q.mu);
  --q.inFlight;
}

void drain() {
  auto& q = queue();
  std::optional<Capture> next;
  {
    std::lock_guard lock(q.mu);
    if (q.waiting.empty()) {
      q.running = false;
      return;
    }
    next.emplace(std::move(q.waiting.front()));
    q.waiting.pop_front();
  }
  auto c = std::make_shared<Capture>(std::move(*next));
  next.reset();
  drogon::async_run([c]() -> Task<void> {
    try {
      co_await analyze(std::move(*c));
    } catch (const std::exception& e) {
      LOG_WARN << "vision: capture dropped: " << e.what();
    }
    c->images.clear();  // zeroed by SecureBuffer, whatever happened
    finished();
    drogon::app().getLoop()->queueInLoop(drain);
  });
}

}  // namespace

bool submit(Capture c) {
  auto& q = queue();
  bool start = false;
  {
    std::lock_guard lock(q.mu);
    if (static_cast<int>(q.waiting.size()) + q.inFlight >= capacity()) return false;
    ++q.inFlight;  // the slot is held from submit until the analysis finished
    q.waiting.push_back(std::move(c));
    if (!q.running) q.running = start = true;
  }
  if (start) drogon::app().getLoop()->queueInLoop(drain);
  return true;
}

int capacity() {
  static const int n = env::getInt("THELIFE_VISION_QUEUE", 8, 1, 64);
  return n;
}

int inUse() {
  auto& q = queue();
  std::lock_guard lock(q.mu);
  return q.inFlight;
}

Task<std::string> analyze(Capture c) {
  std::string userName;
  const auto ctx = co_await contextFor(c, userName);

  const auto p = agent::prompt("jobs/vision_analyze");
  llm::LlmRequest req;
  req.tenantId = c.tenantId;
  req.purpose = "vision.analyze";
  req.priority = llm::Priority::Vision;
  req.vision = true;
  req.reasoningEffort = "low";
  req.promptId = p.id;
  req.promptVersion = p.version + "+" + p.hash;
  req.maxTokens = 1500;
  auto user = llm::Message::user("Context:\n" + json::compact(ctx));
  for (const auto& img : c.images) user.imageDataUris.push_back(dataUri(img));
  req.messages = {llm::Message::system(p.render({{"user_name", userName}})), std::move(user)};
  c.images.clear();  // the base64 copies are all that is left; zeroed below

  // The request (with the only remaining copies of the images, base64) is moved into the gateway and freed with
  // it; the provider's HTTP body is freed after the call. Nothing is logged or stored with image content.
  Json::Value out;
  try {
    out = co_await llm::gateway().structured(std::move(req), "vision_analysis", analysisSchema());
  } catch (const llm::LlmError& e) {
    LOG_WARN << "vision: no analysis for capture " << c.captureId << ": " << llm::toString(e.cls);
    co_return "";
  }

  auto tx = co_await db::TenantTx::beginSystem(c.tenantId);
  const auto dek = co_await fieldcrypt::dekFor(tx);
  Json::Value evidence;
  evidence["capture_id"] = c.captureId;
  evidence["monitors"] = ctx["monitors"];
  evidence["apps"] = out.isMember("apps") ? out["apps"] : Json::Value(Json::arrayValue);
  if (ctx["focused_window"].isObject()) evidence["focused_window"] = ctx["focused_window"];
  const auto id = ids::newUuidV7();
  const int window = std::clamp(c.windowSec, 60, 900);
  co_await tx.exec(
      "INSERT INTO detections (id, tenant_id, created_by_kind, source, device_id, started_at, ended_at, area, "
      "category, summary, confidence, evidence) VALUES ($1::uuid, $2::uuid, 'system', 'vision', $3::uuid, "
      "$4::timestamptz - make_interval(secs => $5::int), $4::timestamptz, $6::area, $7, $8::bytea, $9::real, "
      "$10::bytea)",
      id, c.tenantId, c.deviceId, c.context["captured_at"].asString(), std::to_string(window),
      out["area"].asString(), out["category"].asString(),
      crypto::pgByteaHex(fieldcrypt::seal(dek, c.tenantId, "detections", "summary", out["summary"])),
      std::to_string(out["confidence"].asDouble()),
      crypto::pgByteaHex(fieldcrypt::seal(dek, c.tenantId, "detections", "evidence", evidence)));
  co_await tx.commit();
  co_return id;
}

}  // namespace thelife::vision
