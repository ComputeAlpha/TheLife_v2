// Deterministic chat commands (agent_lld.md §3.4): they work with no model, so the user can still see the day,
// log progress and decide approvals while the assistant is offline. They run as the user, through the registry.
#include <cstdlib>
#include <sstream>

#include "agent/runner.h"
#include "core/errors.h"
#include "tools/registry.h"

namespace thelife::agent {

namespace {

const char* kHelp =
    "Commands:\n"
    "/today: today's plan, numbered\n"
    "/done N: mark item N of /today done\n"
    "/skip N [reason]: skip routine N of /today\n"
    "/tasks: open one-off tasks\n"
    "/approvals: pending approvals, numbered\n"
    "/approve N, /reject N [reason]: decide approval N";

// "2026-11-13T09:30:00Z" (or with offset) → local "15:00" given the user's current UTC offset.
std::string localTime(const std::string& iso, int offsetMinutes) {
  if (iso.size() < 16) return "";
  int h = std::atoi(iso.substr(11, 2).c_str());
  int m = std::atoi(iso.substr(14, 2).c_str());
  int src = 0;  // the instant's own offset
  if (iso.size() >= 25 && (iso[19] == '+' || iso[19] == '-')) {
    src = (std::atoi(iso.substr(20, 2).c_str()) * 60 + std::atoi(iso.substr(23, 2).c_str())) * (iso[19] == '-' ? -1 : 1);
  }
  int total = ((h * 60 + m - src + offsetMinutes) % 1440 + 1440) % 1440;
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d:%02d", total / 60, total % 60);
  return buf;
}

struct Parsed {
  std::string cmd;
  int n = 0;
  std::string rest;
};

Parsed parse(const std::string& text) {
  Parsed p;
  std::istringstream in(text);
  in >> p.cmd;
  std::string num;
  if (in >> num) {
    char* end = nullptr;
    const long v = std::strtol(num.c_str(), &end, 10);
    if (end && *end == '\0' && v > 0 && v < 1000) p.n = static_cast<int>(v);
    else p.rest = num;
  }
  std::string tail;
  std::getline(in, tail);
  const auto b = tail.find_first_not_of(' ');
  if (b != std::string::npos) p.rest += (p.rest.empty() ? "" : " ") + tail.substr(b);
  return p;
}

drogon::Task<Json::Value> call(const auth::Principal& user, const std::string& tool, Json::Value args) {
  tools::InvokeOptions opts;
  auto r = co_await tools::registry().invoke(tool, user, std::move(args), opts);
  co_return r.value;
}

drogon::Task<Json::Value> todayEntries(const auth::Principal& user, const NowInfo& now) {
  Json::Value args;
  args["from"] = now.dayFrom;
  args["to"] = now.dayTo;
  const auto v = co_await call(user, "calendar.view", args);
  co_return v["items"];
}

drogon::Task<Json::Value> pendingApprovals(const auth::Principal& user) {
  Json::Value args;
  args["status"] = "pending";
  args["limit"] = 50;
  const auto v = co_await call(user, "approvals.list", args);
  co_return v["items"];
}

drogon::Task<std::string> dispatch(const auth::Principal& user, const Parsed& p, const NowInfo& now) {
  if (p.cmd == "/help" || p.cmd == "/start") co_return std::string(kHelp);

  if (p.cmd == "/today") {
    const auto items = co_await todayEntries(user, now);
    if (items.empty()) co_return "Nothing planned for " + now.weekday + " " + now.localDay + ".";
    std::string out = now.weekday + " " + now.localDay + ":";
    int i = 0;
    for (const auto& e : items) {
      ++i;
      const bool task = e["kind"].asString() == "task_occurrence";
      out += "\n" + std::to_string(i) + ". ";
      out += e["all_day"].asBool() ? "(all day) " : localTime(e["starts_at"].asString(), now.offsetMinutes) + " ";
      out += e["title"].asString();
      if (task) out += " [" + e["status"].asString() + "]";
      else out += " (" + e["kind"].asString() + ")";
    }
    co_return out;
  }

  if (p.cmd == "/done" || p.cmd == "/skip") {
    if (p.n == 0) co_return "Usage: " + p.cmd + " N (N from /today)";
    const auto items = co_await todayEntries(user, now);
    if (p.n > static_cast<int>(items.size())) co_return "There is no item " + std::to_string(p.n) + " in /today.";
    const auto& e = items[p.n - 1];
    if (e["kind"].asString() != "task_occurrence") co_return "Item " + std::to_string(p.n) + " is not a task.";
    const auto id = e["id"].asString();
    Json::Value getArgs;
    getArgs["id"] = id;
    const auto task = co_await call(user, "tasks.get", getArgs);
    const bool recurring = task["rrule"].isString();
    Json::Value args;
    args["id"] = id;
    if (recurring) {
      args["date"] = e["occurs_on"].asString();
      if (p.cmd == "/skip" && !p.rest.empty()) args["reason"] = p.rest.substr(0, 500);
      co_await call(user, p.cmd == "/done" ? "tasks.complete_occurrence" : "tasks.skip_occurrence", args);
    } else {
      if (p.cmd == "/skip") co_return "Only routines can be skipped; edit or cancel a one-off task instead.";
      args["status"] = "done";
      co_await call(user, "tasks.update", args);
    }
    co_return (p.cmd == "/done" ? "Done: " : "Skipped: ") + e["title"].asString();
  }

  if (p.cmd == "/tasks") {
    Json::Value args;
    args["status"] = "open";
    args["recurring"] = false;
    args["limit"] = 30;
    const auto v = co_await call(user, "tasks.list", args);
    if (v["items"].empty()) co_return std::string("No open tasks.");
    std::string out = "Open tasks:";
    for (const auto& t : v["items"]) {
      out += "\n- " + t["title"].asString();
      if (t["due_at"].isString()) out += " (due " + t["due_at"].asString().substr(0, 10) + ")";
    }
    co_return out;
  }

  if (p.cmd == "/approvals") {
    const auto items = co_await pendingApprovals(user);
    if (items.empty()) co_return std::string("No pending approvals.");
    std::string out = "Pending approvals:";
    int i = 0;
    for (const auto& a : items) {
      std::string summary = a["summary"].asString();
      if (summary.size() > 220) summary = summary.substr(0, 220) + "…";
      out += "\n" + std::to_string(++i) + ". " + summary;
    }
    co_return out;
  }

  if (p.cmd == "/approve" || p.cmd == "/reject") {
    if (p.n == 0) co_return "Usage: " + p.cmd + " N (N from /approvals)";
    const auto items = co_await pendingApprovals(user);
    if (p.n > static_cast<int>(items.size())) co_return "There is no pending approval " + std::to_string(p.n) + ".";
    Json::Value args;
    args["id"] = items[p.n - 1]["id"];
    if (p.cmd == "/reject" && !p.rest.empty()) args["reason"] = p.rest.substr(0, 500);
    const auto a = co_await call(user, p.cmd == "/approve" ? "approvals.approve" : "approvals.reject", args);
    const auto status = a["status"].asString();
    if (status == "approved") co_return std::string("Approved and done.");
    if (status == "rejected") co_return std::string("Rejected; nothing was changed.");
    if (status == "stale") co_return std::string("The item changed since the request, so nothing ran.");
    co_return "Approval is now " + status + (a["error"].isString() ? ": " + a["error"].asString() : "") + ".";
  }

  co_return "Unknown command " + p.cmd + ". Send /help for the list.";
}

}  // namespace

drogon::Task<std::string> runCommand(const auth::Principal& user, const std::string& text, const NowInfo& now) {
  const auto p = parse(text);
  std::optional<std::string> failure;
  try {
    co_return co_await dispatch(user, p, now);
  } catch (const ApiError& e) {
    failure = e.detail().empty() ? e.title() : e.detail();
    if (!e.errors().empty()) *failure += " (" + e.errors().front().path + ": " + e.errors().front().message + ")";
  }
  co_return "Couldn't do that: " + *failure;
}

}  // namespace thelife::agent
