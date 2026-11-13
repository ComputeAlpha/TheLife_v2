// thelife-embedder (backend_lld.md §2.1): bge-m3 embeddings on the GPU behind a Unix domain socket, in its own
// process so a CUDA fault or out-of-memory cannot take the API down.
//   POST /v1/embed  {texts: [≤ 8 strings], max_tokens: ≤ 512}  →  {model, model_version, dim, vectors[][]}
//   GET  /v1/health                                          →  {status, provider, model, model_version, dim}
// Options (or env): --model THELIFE_EMBED_MODEL, --spm THELIFE_EMBED_SPM, --socket THELIFE_EMBEDDER_SOCKET
// (default $XDG_RUNTIME_DIR/thelife/embedder.sock), --model-version, --gpu-mem-mb, --cpu.
#include <json/json.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "model.h"
#include "tokenizer.h"

using namespace thelife::embedder;

namespace {

constexpr std::size_t kMaxBatch = 8;
constexpr std::size_t kMaxTokens = 512;
constexpr std::size_t kMaxBody = 1 << 20;

std::atomic<int> listenFd{-1};
std::string socketPath;

struct Config {
  std::string model, spm, socket, modelVersion = "onnx-fp16-fused@ort1.26";
  bool cpu = false;
  std::size_t gpuMemMb = 2560;
};

std::string env(const char* k, const std::string& d = {}) {
  const char* v = std::getenv(k);
  return v && *v ? v : d;
}

Config parseArgs(int argc, char** argv) {
  Config c;
  c.model = env("THELIFE_EMBED_MODEL");
  c.spm = env("THELIFE_EMBED_SPM");
  const auto runtime = env("XDG_RUNTIME_DIR", "/tmp");
  c.socket = env("THELIFE_EMBEDDER_SOCKET", runtime + "/thelife/embedder.sock");
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(a + " needs a value");
      return argv[++i];
    };
    if (a == "--model") c.model = next();
    else if (a == "--spm") c.spm = next();
    else if (a == "--socket") c.socket = next();
    else if (a == "--model-version") c.modelVersion = next();
    else if (a == "--gpu-mem-mb") c.gpuMemMb = std::stoul(next());
    else if (a == "--cpu") c.cpu = true;
    else throw std::invalid_argument("unknown option " + a);
  }
  if (c.model.empty() || c.spm.empty()) throw std::invalid_argument("--model and --spm are required");
  return c;
}

std::string jsonText(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  b["precision"] = 7;
  return Json::writeString(b, v);
}

struct Response {
  int status;
  std::string body;
};

Response problem(int status, const std::string& detail) {
  Json::Value p;
  p["status"] = status;
  p["title"] = status == 400 ? "Bad Request" : status == 404 ? "Not Found" : "Error";
  p["detail"] = detail;
  return {status, jsonText(p)};
}

class Service {
 public:
  Service(const Config& c, Tokenizer& tok, Model& model) : cfg_(c), tok_(tok), model_(model) {}

  Response handle(const std::string& method, const std::string& path, const std::string& body) {
    if (path == "/v1/health" && method == "GET") {
      Json::Value h;
      h["status"] = "ok";
      h["provider"] = model_.provider();
      h["model"] = "bge-m3";
      h["model_version"] = cfg_.modelVersion;
      h["dim"] = model_.dim();
      return {200, jsonText(h)};
    }
    if (path != "/v1/embed") return problem(404, "unknown path");
    if (method != "POST") return problem(400, "POST required");
    Json::Value req;
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream in(body);
    if (!Json::parseFromStream(rb, in, &req, &errs) || !req.isObject() || !req["texts"].isArray()) {
      return problem(400, "body must be {texts: [...]}");
    }
    const auto& texts = req["texts"];
    if (texts.empty() || texts.size() > kMaxBatch) return problem(400, "1 to 8 texts per call");
    const auto maxTokens = req.get("max_tokens", static_cast<Json::UInt>(kMaxTokens)).asUInt();
    if (maxTokens < 8 || maxTokens > kMaxTokens) return problem(400, "max_tokens must be 8..512");
    std::vector<std::vector<int64_t>> batch;
    for (const auto& t : texts) {
      if (!t.isString()) return problem(400, "texts must be strings");
      batch.push_back(tok_.encode(t.asString(), maxTokens));
    }
    std::vector<std::vector<float>> vecs;
    {
      std::lock_guard lock(gpu_);  // one session, one inference at a time
      vecs = model_.embed(batch);
    }
    Json::Value out;
    out["model"] = "bge-m3";
    out["model_version"] = cfg_.modelVersion;
    out["dim"] = model_.dim();
    out["vectors"] = Json::Value(Json::arrayValue);
    for (const auto& v : vecs) {
      Json::Value row(Json::arrayValue);
      for (const float x : v) row.append(x);
      out["vectors"].append(row);
    }
    return {200, jsonText(out)};
  }

 private:
  const Config& cfg_;
  Tokenizer& tok_;
  Model& model_;
  std::mutex gpu_;
};

// Minimal HTTP/1.1 over a Unix socket: Content-Length bodies, keep-alive, nothing else.
void serveConnection(int fd, Service& svc) {
  timeval tv{60, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::string buf;
  char chunk[65536];
  for (;;) {
    std::size_t headerEnd;
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
      const auto n = ::read(fd, chunk, sizeof chunk);
      if (n <= 0 || buf.size() > 64 * 1024) return close(fd), void();
      buf.append(chunk, static_cast<std::size_t>(n));
    }
    std::istringstream head(buf.substr(0, headerEnd));
    std::string method, path, version, line;
    head >> method >> path >> version;
    std::getline(head, line);
    std::size_t length = 0;
    bool keepAlive = version == "HTTP/1.1";
    while (std::getline(head, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      std::string value = line.substr(colon + 1);
      value.erase(0, value.find_first_not_of(' '));
      if (name == "content-length") length = std::stoul(value);
      if (name == "connection") keepAlive = value != "close";
    }
    if (length > kMaxBody) return close(fd), void();
    while (buf.size() < headerEnd + 4 + length) {
      const auto n = ::read(fd, chunk, sizeof chunk);
      if (n <= 0) return close(fd), void();
      buf.append(chunk, static_cast<std::size_t>(n));
    }
    const auto body = buf.substr(headerEnd + 4, length);
    buf.erase(0, headerEnd + 4 + length);
    Response r;
    try {
      r = svc.handle(method, path, body);
    } catch (const std::exception& e) {
      r = problem(500, e.what());
    }
    const std::string reason = r.status == 200 ? "OK" : r.status == 400 ? "Bad Request" : r.status == 404 ? "Not Found" : "Internal Server Error";
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason + "\r\nContent-Type: " +
                      (r.status == 200 ? "application/json" : "application/problem+json") +
                      "\r\nContent-Length: " + std::to_string(r.body.size()) +
                      (keepAlive ? "\r\n" : "\r\nConnection: close\r\n") + "\r\n" + r.body;
    std::size_t sent = 0;
    while (sent < out.size()) {
      const auto n = ::write(fd, out.data() + sent, out.size() - sent);
      if (n <= 0) return close(fd), void();
      sent += static_cast<std::size_t>(n);
    }
    if (!keepAlive) return close(fd), void();
  }
}

void stop(int) {
  const int fd = listenFd.exchange(-1);
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR), ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto cfg = parseArgs(argc, argv);
    Tokenizer tok(cfg.spm);
    ModelOptions mo;
    mo.path = cfg.model;
    mo.cpu = cfg.cpu;
    mo.gpuMemLimit = cfg.gpuMemMb * 1024 * 1024;
    Model model(mo);
    Service svc(cfg, tok, model);

    // Owner-only directory; the socket itself is 0660 (backend_lld.md §2.1).
    const auto dir = std::filesystem::path(cfg.socket).parent_path();
    std::filesystem::create_directories(dir);
    ::chmod(dir.c_str(), 0700);
    ::unlink(cfg.socket.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (cfg.socket.size() >= sizeof addr.sun_path) throw std::runtime_error("socket path too long");
    std::strncpy(addr.sun_path, cfg.socket.c_str(), sizeof addr.sun_path - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      throw std::runtime_error("bind " + cfg.socket + ": " + std::strerror(errno));
    }
    ::chmod(cfg.socket.c_str(), 0660);
    if (::listen(fd, 16) != 0) throw std::runtime_error("listen failed");
    listenFd = fd;
    socketPath = cfg.socket;
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    std::cout << "thelife-embedder ready on " << cfg.socket << " (" << model.provider() << ", dim " << model.dim()
              << ")" << std::endl;
    for (;;) {
      const int lfd = listenFd.load();
      if (lfd < 0) break;
      const int c = ::accept(lfd, nullptr, nullptr);
      if (c < 0) {
        if (listenFd.load() < 0) break;
        continue;
      }
      std::thread(serveConnection, c, std::ref(svc)).detach();
    }
    ::unlink(socketPath.c_str());
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "thelife-embedder: " << e.what() << "\n";
    return 1;
  }
}
