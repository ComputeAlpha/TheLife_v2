#include "support/fake_llm.h"

#include <signal.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

#include "support/http.h"
#include "support/server_process.h"

extern char** environ;

namespace thelife::test {

namespace {

// Spawns a fake server that binds port 0 and prints "<name> listening on <port>"; returns its port.
int spawnFake(pid_t& pid, const std::string& name, std::vector<std::string> args) {
  int out[2];
  if (::pipe(out) != 0) throw std::runtime_error("pipe failed");
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, out[0]);
  args.insert(args.begin(), {"python3", "-I"});
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  const int rc = ::posix_spawnp(&pid, "python3", &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(out[1]);
  if (rc != 0) {
    ::close(out[0]);
    throw std::runtime_error("failed to spawn " + name);
  }
  std::string line;
  char c;
  while (line.find('\n') == std::string::npos && ::read(out[0], &c, 1) == 1) line += c;
  ::close(out[0]);
  const auto at = line.rfind(' ');
  if (line.rfind(name + " listening on ", 0) != 0 || at == std::string::npos) {
    throw std::runtime_error(name + " did not start: " + line);
  }
  return std::stoi(line.substr(at + 1));
}

}  // namespace

FakeTelegram::FakeTelegram() {
  port_ = spawnFake(pid_, "fake-telegram", {THELIFE_FAKE_TELEGRAM, "--port", "0", "--token", kToken});
  if (httpGet(root() + "/_fake/health").status != 200) throw std::runtime_error("fake-telegram not healthy");
}

FakeTelegram::~FakeTelegram() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  ::waitpid(pid_, nullptr, 0);
}

int64_t FakeTelegram::push(const Json::Value& update) {
  const auto r = sendJson("POST", root() + "/_fake/update", update);
  if (r.status != 200) throw std::runtime_error("fake-telegram update");
  return r.json["update_id"].asInt64();
}

int64_t FakeTelegram::message(int64_t chatId, const std::string& text, const std::string& username) {
  Json::Value u;
  auto& m = u["message"];
  m["message_id"] = static_cast<Json::Int64>(nextMessageId_++);
  m["chat"]["id"] = static_cast<Json::Int64>(chatId);
  m["chat"]["type"] = "private";
  m["from"]["id"] = static_cast<Json::Int64>(chatId);
  m["from"]["username"] = username;
  m["text"] = text;
  return push(u);
}

int64_t FakeTelegram::callback(int64_t chatId, const std::string& data, int64_t messageId) {
  Json::Value u;
  auto& q = u["callback_query"];
  q["id"] = "cb" + std::to_string(nextMessageId_++);
  q["from"]["id"] = static_cast<Json::Int64>(chatId);
  q["data"] = data;
  q["message"]["message_id"] = static_cast<Json::Int64>(messageId);
  q["message"]["chat"]["id"] = static_cast<Json::Int64>(chatId);
  q["message"]["text"] = "Needs your approval";
  return push(u);
}

Json::Value FakeTelegram::calls() const { return httpGet(root() + "/_fake/calls").json; }

Json::Value FakeTelegram::calls(const std::string& method) const {
  Json::Value out(Json::arrayValue);
  for (const auto& c : calls()) {
    if (c["method"].asString() == method) out.append(c["body"]);
  }
  return out;
}

void FakeTelegram::failNext(const std::string& method, int count) {
  Json::Value b;
  b["method"] = method;
  b["count"] = count;
  sendJson("POST", root() + "/_fake/fail", b);
}

FakeLlm::FakeLlm() {
  // The fake binds port 0 itself and reports the port on stdout. Picking a "free" port here and passing it in
  // raced under parallel tests: another test's fake could take the port first, our health check then reached the
  // wrong server, and this test's scripts went to another test.
  int out[2];
  if (::pipe(out) != 0) throw std::runtime_error("pipe failed");
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, out[0]);
  std::string py = "python3", isolated = "-I", script = THELIFE_FAKE_LLM, portFlag = "--port", port = "0";
  char* argv[] = {py.data(), isolated.data(), script.data(), portFlag.data(), port.data(), nullptr};
  const int rc = ::posix_spawnp(&pid_, "python3", &fa, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(out[1]);
  if (rc != 0) {
    ::close(out[0]);
    throw std::runtime_error("failed to spawn fake-llm");
  }
  std::string line;
  char c;
  while (line.find('\n') == std::string::npos && ::read(out[0], &c, 1) == 1) line += c;
  ::close(out[0]);
  const auto at = line.rfind(' ');
  if (line.rfind("fake-llm listening on ", 0) != 0 || at == std::string::npos) {
    throw std::runtime_error("fake-llm did not start: " + line);
  }
  port_ = std::stoi(line.substr(at + 1));
  if (httpGet(root() + "/_fake/health").status != 200) throw std::runtime_error("fake-llm not healthy");
}

FakeLlm::~FakeLlm() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  ::waitpid(pid_, nullptr, 0);
}

void FakeLlm::script(const Json::Value& responses) {
  Json::Value body;
  body["responses"] = responses;
  if (sendJson("POST", root() + "/_fake/script", body).status != 200) throw std::runtime_error("fake-llm script");
}

void FakeLlm::reset() { sendJson("POST", root() + "/_fake/reset", Json::Value(Json::objectValue)); }

void FakeLlm::webScript(const Json::Value& responses) {
  Json::Value body;
  body["responses"] = responses;
  if (sendJson("POST", root() + "/_fake/web_script", body).status != 200) throw std::runtime_error("fake-llm web script");
}

Json::Value FakeLlm::webRequests() const { return httpGet(root() + "/_fake/web_requests").json; }

Json::Value FakeLlm::requests() const { return httpGet(root() + "/_fake/requests").json; }

FakeEmbedder::FakeEmbedder() {
  char tmpl[] = "/tmp/thelife-fake-embedder-XXXXXX";
  if (!::mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
  dir_ = tmpl;
  socket_ = dir_ + "/embedder.sock";
  std::string py = "python3", isolated = "-I", script = THELIFE_FAKE_EMBEDDER, flag = "--socket", sock = socket_;
  char* argv[] = {py.data(), isolated.data(), script.data(), flag.data(), sock.data(), nullptr};
  if (::posix_spawnp(&pid_, "python3", nullptr, nullptr, argv, environ) != 0) {
    throw std::runtime_error("failed to spawn fake-embedder");
  }
  for (int i = 0; i < 200; ++i) {
    if (std::filesystem::exists(socket_)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  throw std::runtime_error("fake-embedder did not start");
}

void FakeEmbedder::stop() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  ::waitpid(pid_, nullptr, 0);
  pid_ = -1;
  std::filesystem::remove(socket_);
}

FakeEmbedder::~FakeEmbedder() {
  stop();
  std::filesystem::remove_all(dir_);
}

}  // namespace thelife::test
