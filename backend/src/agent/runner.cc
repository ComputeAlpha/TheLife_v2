#include "agent/runner.h"

#include <drogon/drogon.h>

#include <deque>
#include <map>
#include <mutex>
#include <set>

#include "agent/events.h"
#include "agent/prompt.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "llm/classifier.h"
#include "llm/gateway.h"
#include "db/job_store.h"
#include "memory/embedder.h"
#include "memory/registry.h"
#include "memory/semantic.h"
#include "proactive/notifier.h"
#include "telegram/bot.h"
#include "tools/registry.h"

namespace thelife::agent {

namespace {

using auth::Principal;
using auth::PrincipalKind;

// ------------------------------------------------------------------------------------------------ queue

struct Lane {
  std::deque<TurnRequest> queue;
  bool running = false;
};
std::mutex lanesMu;
std::map<std::string, Lane> lanes;

drogon::Task<void> markFailed(const TurnRequest& r, const std::string& why);

drogon::Task<void> drain(std::string key) {
  for (;;) {
    TurnRequest r;
    {
      std::lock_guard lock(lanesMu);
      auto& lane = lanes[key];
      if (lane.queue.empty()) {
        lane.running = false;
        co_return;
      }
      r = std::move(lane.queue.front());
      lane.queue.pop_front();
    }
    std::string failure;
    try {
      co_await runTurn(r);
    } catch (const std::exception& e) {
      failure = e.what();
    }
    if (!failure.empty()) {
      LOG_ERROR << "agent turn failed: " << failure;
      try {
        co_await markFailed(r, "internal error");
      } catch (const std::exception& e) {
        LOG_ERROR << "agent turn not marked failed: " << e.what();
      }
    }
  }
}

Principal principalOf(const TurnRequest& r, PrincipalKind kind) {
  Principal p;
  p.kind = kind;
  p.tenantId = r.tenantId;
  p.userId = r.userId;
  return p;
}

// ------------------------------------------------------------------------------------------------ helpers

struct HubSink : llm::StreamSink {
  std::string userId, turnId;
  void delta(std::string_view t) override {
    Json::Value d;
    d["turn_id"] = turnId;
    d["text"] = std::string(t);
    EventHub::instance().publish(userId, "token", d);
  }
  void reset() override {
    Json::Value d;
    d["turn_id"] = turnId;
    EventHub::instance().publish(userId, "reset", d);
  }
};

std::string agentName(const std::string& tool) {
  std::string n = tool;
  for (auto& c : n) {
    if (c == '.') c = '_';
  }
  return n;
}

const std::map<std::string, std::string>& agentNameMap() {
  static const std::map<std::string, std::string> m = [] {
    std::map<std::string, std::string> out;
    for (const auto& [name, def] : tools::embeddedManifest().tools) {
      if (def.exposure.count("agent")) out[agentName(name)] = name;
    }
    return out;
  }();
  return m;
}

// "  - planning: activities, events, goals, plans, tasks" per loadable group.
std::string groupsIndex() {
  std::string out;
  for (const auto& [group, names] : tools::embeddedManifest().agentGroups) {
    if (group == "core") continue;
    std::set<std::string> prefixes;
    for (const auto& n : names) prefixes.insert(n.substr(0, n.find('.')));
    std::string list;
    for (const auto& p : prefixes) list += (list.empty() ? "" : ", ") + p;
    out += "  - " + group + ": " + list + "\n";
  }
  if (!out.empty()) out.pop_back();
  return out;
}

std::string truncate(std::string s, std::size_t n) {
  if (s.size() > n) s = s.substr(0, n) + "…";
  return s;
}

// Cross-channel digest (§7.3): only while Telegram is linked (otherwise there is no other channel), after a turn
// that changed data or once 6+ messages arrived since the last digest. Coalesced per user and channel.
drogon::Task<void> maybeRefreshDigest(db::TenantTx& tx, const TurnRequest& r, const std::string& turnId) {
  const auto s = co_await tx.exec(
      "SELECT EXISTS (SELECT 1 FROM telegram_links WHERE user_id = $1::uuid AND revoked_at IS NULL) AS linked, "
      "  (SELECT count(*) FROM messages m JOIN turns t ON t.conversation_id = m.conversation_id WHERE t.id = $2::uuid "
      "     AND m.role IN ('user', 'assistant') AND m.kind IN ('normal', 'deterministic') AND m.content <> '')::int AS n, "
      "  coalesce((SELECT g.messages_seen FROM channel_digests g JOIN conversations c ON c.user_id = g.user_id "
      "     AND c.channel = g.channel AND c.local_day = g.local_day JOIN turns t ON t.conversation_id = c.id "
      "     WHERE t.id = $2::uuid), 0) AS seen, "
      "  coalesce((SELECT string_agg(tool, ',') FROM agent_actions WHERE turn_id = $2::uuid "
      "     AND outcome IN ('executed', 'pending')), '') AS tools",
      r.userId, turnId);
  if (s.empty() || !s[0]["linked"].as<bool>()) co_return;
  bool wrote = false;
  const auto tools = s[0]["tools"].as<std::string>();
  const auto& defs = tools::registry().manifest().tools;
  for (std::size_t at = 0; at < tools.size() && !wrote;) {
    auto end = tools.find(',', at);
    if (end == std::string::npos) end = tools.size();
    const auto it = defs.find(tools.substr(at, end - at));
    wrote = it != defs.end() && it->second.effect != tools::Effect::Read;
    at = end + 1;
  }
  if (!wrote && s[0]["n"].as<int>() - s[0]["seen"].as<int>() < 6) co_return;
  db::jobs::NewJob j;
  j.kind = "digest.refresh";
  j.payload["user_id"] = r.userId;
  j.payload["channel"] = r.channel;
  j.dedupeKey = "digest:" + r.userId + ":" + r.channel;
  j.delaySec = 5;  // a burst of turns makes one digest
  co_await db::jobs::enqueue(tx, std::move(j));
}

drogon::Task<void> finishTurn(const TurnRequest& r, const std::string& turnId, const std::string& status,
                              const std::string& error, int steps, int calls, const std::string& replyId) {
  auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
  std::vector<std::optional<std::string>> params{turnId, status,
                                                 error.empty() ? std::nullopt : std::optional(error),
                                                 std::to_string(steps), std::to_string(calls)};
  co_await tx.execParams(
      "UPDATE turns SET status = $2, error = $3, steps = $4::int, tool_calls = $5::int, finished_at = now(), "
      "updated_at = now() WHERE id = $1::uuid",
      std::move(params));
  co_await memory::enqueueSync(tx);  // embed the new messages (coalesced per tenant)
  co_await maybeRefreshDigest(tx, r, turnId);
  co_await tx.commit();
  Json::Value d;
  d["turn_id"] = turnId;
  d["status"] = status;
  d["message_id"] = replyId.empty() ? Json::Value() : Json::Value(replyId);
  EventHub::instance().publish(r.userId, "turn.finished", d);
}

drogon::Task<void> markFailed(const TurnRequest& r, const std::string& why) {
  if (r.turnId.empty()) co_return;
  co_await finishTurn(r, r.turnId, "failed", why, 0, 0, "");
}

// Telegram-channel turns answer in the chat (agent_lld.md §7.2): plain text; an approval gets its buttons.
drogon::Task<void> toTelegram(const TurnRequest& r, std::string text, std::string approvalId = {}) {
  if (r.channel != "telegram" || !telegram::enabled()) co_return;
  std::optional<int64_t> chat;
  std::string summary;
  {
    auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
    chat = co_await telegram::chatOf(tx, r.userId);
    if (chat && !approvalId.empty()) {
      const auto a = co_await tx.exec("SELECT summary FROM approvals WHERE id = $1::uuid", approvalId);
      if (!a.empty()) summary = a[0]["summary"].as<std::string>();
    }
    co_await tx.commit();
  }
  if (!chat) co_return;
  if (approvalId.empty()) co_await telegram::send(*chat, std::move(text));
  else co_await telegram::sendApproval(*chat, r.userId, approvalId, summary);
}

drogon::Task<std::string> say(const TurnRequest& r, const std::string& conversationId, const std::string& turnId,
                              const std::string& text, const std::string& kind) {
  auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
  StoredMessage m;
  m.conversationId = conversationId;
  m.turnId = turnId;
  m.role = "assistant";
  m.kind = kind;
  m.content = text;
  const auto id = co_await insertMessage(tx, std::move(m));
  co_await tx.commit();
  Json::Value d;
  d["turn_id"] = turnId;
  d["message_id"] = id;
  d["kind"] = kind;
  d["text"] = text;
  EventHub::instance().publish(r.userId, "message", d);
  co_await toTelegram(r, text);
  co_return id;
}

// Local wall-clock text for an instant, in the user's zone.
drogon::Task<std::string> localText(const TurnRequest& r, std::optional<llm::Clock::time_point> t) {
  if (!t) co_return std::string();
  auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::User));
  const auto rows = co_await tx.exec(
      "SELECT to_char(to_timestamp($1::bigint) AT TIME ZONE u.timezone, 'Dy DD Mon HH24:MI') AS t FROM users u "
      "WHERE u.id = $2::uuid",
      std::to_string(llm::Clock::to_time_t(*t)), r.userId);
  co_await tx.commit();
  co_return rows.empty() ? std::string() : rows[0]["t"].as<std::string>();
}

// ------------------------------------------------------------------------------------------------ context

struct TurnState {
  std::string conversationId, turnId, userMessageId;
  std::set<std::string> groups{"core"};
  std::map<std::string, int> webCalls;  // per-turn caps for web.search / web.fetch
  bool untrusted = false;               // web content entered the context
  std::optional<std::string> related;   // semantic recall for the user's message, computed once per turn
  std::string approvalNote;  // approval, proactive and deferred-reply turns: the event that started the turn
  bool proactive = false;    // the agent started it: the final message goes through the Notifier gate
  bool silent = false;
  bool urgent = false;       // notify.urgent was called
  bool noMessage = false;    // agent.no_message was called
  std::string gateReason;    // why a proactive message was held
};

drogon::Task<llm::LlmRequest> buildRequest(const TurnRequest& r, TurnState& st) {
  auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
  const auto now = co_await loadNow(tx, r.userId);
  const auto memory = co_await loadWorkingMemory(tx, r.userId, r.channel, now.localDay);
  const auto pending = co_await tx.exec(
      "SELECT summary FROM approvals WHERE status = 'pending' AND user_id = $1::uuid ORDER BY created_at LIMIT 5",
      r.userId);
  const auto pendingCount = co_await tx.exec(
      "SELECT count(*) AS n FROM approvals WHERE status = 'pending' AND user_id = $1::uuid", r.userId);
  const auto profile = co_await memory::profileFacts(tx, r.userId, 30);
  const auto days = co_await tx.exec(
      "SELECT to_char(period_start, 'YYYY-MM-DD') AS day, content FROM episodic_summaries "
      "WHERE user_id = $1::uuid AND channel = $2 AND period = 'day' "
      "  AND period_start BETWEEN $3::date - 8 AND $3::date - 2 ORDER BY period_start",
      r.userId, r.channel, now.localDay);
  // Everything up to here is in this call's context: a queued turn for a message older than this is merged.
  co_await tx.exec(
      "UPDATE turns SET seen_until = (SELECT max(created_at) FROM messages WHERE conversation_id = $2::uuid), "
      "updated_at = now() WHERE id = $1::uuid",
      st.turnId, st.conversationId);
  co_await tx.commit();

  auto mtx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
  const auto& prompt = agentSystemPrompt();
  llm::LlmRequest req;
  req.tenantId = r.tenantId;
  req.purpose = "agent.turn";
  req.priority = r.trigger == "user_message" ? llm::Priority::Interactive : llm::Priority::Proactive;
  req.conversationId = st.conversationId;
  req.promptId = prompt.id;
  req.promptVersion = prompt.version + "+" + prompt.hash;
  req.maxTokens = 2048;
  req.messages.push_back(llm::Message::system(
      prompt.render({{"user_name", now.userName}, {"timezone", now.timezone}, {"groups", groupsIndex()}})));

  std::string nowNote = "Now: " + now.weekday + " " + now.localNow + " (local day " + now.localDay +
                        ", the day starts at " + std::to_string(now.dayStartHour) + ":00). Channel: " + r.channel +
                        ". Agent writes: " + (now.agentWritesEnabled ? "enabled" : "disabled by the user") +
                        ". Pending approvals: " + pendingCount[0]["n"].as<std::string>() + ".";
  for (const auto& p : pending) nowNote += "\n- " + truncate(p["summary"].as<std::string>(), 200);

  // Memory (§6.1): core profile facts, then the Now note, then earlier days and facts about what the latest
  // message mentions. Yesterday and today are in the working memory verbatim.
  std::string lastUser;
  for (const auto& m : memory) {
    if (m.role == "user") lastUser = m.content;
  }
  if (!profile.empty()) {
    std::string note = "What you know about " + now.userName + " (long-term memory):";
    for (const auto& f : profile) note += "\n- " + f;
    req.messages.push_back(llm::Message::system(note));
  }
  req.messages.push_back(llm::Message::system(nowNote));
  // The other channel today, as a short digest (§7.3): its transcript stays there.
  {
    const auto other = r.channel == "telegram" ? "web" : "telegram";
    const auto dg = co_await mtx.exec(
        "SELECT content, to_char(updated_at AT TIME ZONE (SELECT timezone FROM users WHERE id = $1::uuid), 'HH24:MI') "
        "AS at FROM channel_digests WHERE user_id = $1::uuid AND channel = $2 AND local_day = $3::date",
        r.userId, std::string(other), now.localDay);
    if (!dg.empty()) {
      req.messages.push_back(llm::Message::system(
          std::string("Today on the ") + other + " channel (digest as of " + dg[0]["at"].as<std::string>() +
          "; the data it mentions is already saved):\n" + dg[0]["content"].as<std::string>()));
    }
  }
  std::string recall;
  for (const auto& d : days) {
    recall += "\n" + d["day"].as<std::string>() + ": " + truncate(d["content"].as<std::string>(), 2500);
  }
  if (!recall.empty()) recall = "Earlier days on this channel (summaries):" + recall;
  const auto mentioned = co_await memory::factsMentionedIn(mtx, r.userId, lastUser, 10);
  // Semantic recall (§6.1 section 6): older items close in meaning to the user's message. Before yesterday only,
  // since yesterday and today are in the context verbatim. Skipped quietly without an embedder.
  if (!st.related) {
    st.related = std::string();
    if (r.trigger == "user_message" && lastUser.size() >= 12) {
      std::vector<std::string> queryText{lastUser};
      const std::vector<std::string> kinds{"message", "summary", "activity"};
      if (const auto e = co_await memory::embed(queryText)) {
        const auto cut = co_await mtx.exec(
            "SELECT ($1::timestamptz - interval '1 day')::text AS t, (now() - interval '3 months')::text AS f",
            now.dayFrom);
        const auto hits = co_await memory::semanticSearch(mtx, r.userId, e->vectors[0], kinds,
                                                          cut[0]["f"].as<std::string>(), cut[0]["t"].as<std::string>(), 4);
        for (const auto& h : hits) {
          if (h.score < memory::minRecallScore()) continue;
          *st.related += "\n- " + h.at.substr(0, 10) + " (" + h.kind + "): " + truncate(h.text, 300);
        }
      }
    }
  }
  co_await mtx.commit();
  if (!mentioned.empty()) {
    recall += std::string(recall.empty() ? "" : "\n\n") + "Remembered about things in the latest message:";
    for (const auto& f : mentioned) recall += "\n- " + f;
  }
  if (!st.related->empty()) {
    recall += std::string(recall.empty() ? "" : "\n\n") + "Related from earlier (by meaning):" + *st.related;
  }
  if (!recall.empty()) req.messages.push_back(llm::Message::system(recall));

  // Working memory, verbatim; oldest dropped first when over budget (full compaction: agent_lld.md §6.3).
  std::vector<llm::Message> wm;
  for (const auto& m : memory) {
    if (m.role == "user") wm.push_back(llm::Message::user(m.content));
    else if (m.role == "assistant") wm.push_back(llm::Message::assistant(m.content, m.toolCalls));
    else wm.push_back(llm::Message::tool(m.toolCallId, m.content));
  }
  const auto& providers = llm::gateway().config().providers;
  const std::size_t ctxTokens = providers.empty() ? 32768 : static_cast<std::size_t>(providers.front().caps.ctxTokens);
  const std::size_t budgetChars = (ctxTokens > 12000 ? ctxTokens - 12000 : 4000) * 3;  // ~3 chars per token, safe
  auto size = [](const llm::Message& m) {
    std::size_t n = m.content.size() + 16;
    for (const auto& c : m.toolCalls) n += c.arguments.size() + c.name.size() + 16;
    return n;
  };
  std::size_t total = 0;
  for (const auto& m : wm) total += size(m);
  std::size_t start = 0;
  while (total > budgetChars && start < wm.size()) total -= size(wm[start++]);
  // Never open on a tool result whose call was cut off. (Skipping to the first user message, as before, dropped
  // everything on a day without one: a proactive turn then lost its own steps and called context_now forever.)
  while (start < wm.size() && wm[start].role == "tool") ++start;
  // The event that started a non-user turn goes right before the turn's own steps (rebuilt from the database each
  // step), so after a tool result the model continues instead of seeing the request again. Chat models expect a
  // user turn there (on an empty morning there is none, and glm-5.3-flash then returns nothing): it is a user
  // message, labelled so the model does not take it for something the user typed.
  std::size_t turnStart = wm.size();
  for (std::size_t i = 0; i < memory.size(); ++i) {
    if (memory[i].turnId == st.turnId) {
      turnStart = i;
      break;
    }
  }
  start = std::min(start, turnStart);  // the running turn's own steps are never trimmed
  const std::size_t noteAt = turnStart;
  for (std::size_t i = start; i <= wm.size(); ++i) {
    if (i == noteAt && !st.approvalNote.empty()) {
      req.messages.push_back(llm::Message::user("[Event from TheLife, not typed by the user] " + st.approvalNote));
    }
    if (i < wm.size()) req.messages.push_back(wm[i]);
  }



  const auto& man = tools::embeddedManifest();
  for (const auto& g : st.groups) {
    const auto it = man.agentGroups.find(g);
    if (it == man.agentGroups.end()) continue;
    for (const auto& name : it->second) {
      const auto& def = man.tools.at(name);
      req.tools.push_back(llm::ToolSpec{agentName(name), def.description, def.inputSchema});
    }
  }
  co_return req;
}

// ------------------------------------------------------------------------------------------------ tool calls

struct CallOutcome {
  Json::Value result;
  std::string decision, outcome;
  std::optional<std::string> approvalId, error;
};

drogon::Task<CallOutcome> execute(const TurnRequest& r, TurnState& st, const llm::ToolCallOut& call) {
  CallOutcome out;
  const auto& names = agentNameMap();
  const auto it = names.find(call.name);
  const auto& man = tools::embeddedManifest();
  auto fail = [&](const std::string& decision, const std::string& msg) {
    out.decision = decision;
    out.outcome = decision == "deny" ? "denied" : "failed";
    out.error = msg;
    out.result["error"] = msg;
  };
  if (it == names.end() || !man.tools.at(it->second).agentGroup ||
      !st.groups.count(*man.tools.at(it->second).agentGroup)) {
    fail("invalid", "Unknown or unloaded tool " + call.name + ". Load its group with agent_load_tools first.");
    co_return out;
  }
  const auto& tool = it->second;
  Json::Value args;
  try {
    args = json::parse(call.arguments.empty() ? "{}" : call.arguments);
  } catch (const std::exception&) {
  }
  if (!args.isObject()) {
    fail("invalid", "The arguments must be a JSON object.");
    co_return out;
  }

  if (tool == "agent.load_tools") {
    const auto group = args["group"].asString();
    if (!man.agentGroups.count(group) || group == "core") {
      fail("invalid", "Unknown group " + group + ".");
      co_return out;
    }
    st.groups.insert(group);
    out.decision = "allow";
    out.outcome = "executed";
    out.result["loaded"] = Json::Value(Json::arrayValue);
    for (const auto& n : man.agentGroups.at(group)) out.result["loaded"].append(agentName(n));
    co_return out;
  }

  if (tool == "web.search" || tool == "web.fetch") {
    if (++st.webCalls[tool] > kMaxWebCallsPerTool) {
      fail("invalid", "Limit of " + std::to_string(kMaxWebCallsPerTool) + " " + tool +
                          " calls per turn reached; answer with what you have.");
      co_return out;
    }
  }
  // Turn-level signals, handled here (§4.3, §5.1).
  if (tool == "agent.no_message" || tool == "notify.urgent") {
    (tool == "agent.no_message" ? st.noMessage : st.urgent) = true;
    out.decision = "allow";
    out.outcome = "executed";
    out.result["ok"] = true;
    co_return out;
  }
  tools::InvokeOptions opts;
  opts.requestId = st.turnId;
  opts.untrustedContext = st.untrusted;
  opts.conversationId = st.conversationId;
  opts.turnId = st.turnId;
  const bool fromUser = r.trigger == "user_message" || r.trigger == "deferred_reply";
  opts.userTurn = fromUser;
  opts.origin = fromUser ? "user_stated" : "inferred";
  if (!st.userMessageId.empty()) opts.sourceMessageId = st.userMessageId;
  std::optional<ApiError> apiErr;
  try {
    auto res = co_await tools::registry().invoke(tool, principalOf(r, PrincipalKind::Agent), args, opts);
    if (res.outcome == tools::Outcome::PendingApproval) {
      out.decision = "approval";
      out.outcome = "pending";
      out.approvalId = res.value["approval_id"].asString();
      out.result = res.value;
      out.result["note"] = "Held for the user's approval. Tell the user; do not retry.";
      Json::Value d;
      d["turn_id"] = st.turnId;
      d["approval_id"] = *out.approvalId;
      d["tool"] = tool;
      EventHub::instance().publish(r.userId, "approval.requested", d);
      co_await toTelegram(r, "", *out.approvalId);
    } else {
      out.decision = "allow";
      out.outcome = "executed";
      out.result = res.value.isNull() ? Json::Value(Json::objectValue) : res.value;
      if (tool == "web.search" || tool == "web.fetch") st.untrusted = true;
      if (res.value.isNull()) out.result["ok"] = true;
    }
  } catch (const ApiError& e) {
    apiErr = e;
  } catch (const std::exception& e) {
    LOG_ERROR << "agent tool " << tool << " failed: " << e.what();
    fail("error", "Internal error while running " + tool + ".");
  }
  if (apiErr) {
    const int s = apiErr->status();
    fail(s == 403 ? "deny" : (s == 422 || s == 400) ? "invalid" : "error",
         apiErr->title() + (apiErr->detail().empty() ? "" : ": " + apiErr->detail()));
    out.result["error"] = Json::Value(Json::objectValue);
    out.result["error"]["status"] = s;
    out.result["error"]["title"] = apiErr->title();
    if (!apiErr->detail().empty()) out.result["error"]["detail"] = apiErr->detail();
    for (const auto& fe : apiErr->errors()) {
      Json::Value e;
      e["path"] = fe.path;
      e["message"] = fe.message;
      out.result["error"]["errors"].append(e);
    }
  }
  co_return out;
}

drogon::Task<void> recordStep(const TurnRequest& r, const TurnState& st, const std::string& assistantId,
                              const llm::ToolCallOut& call, const CallOutcome& o) {
  auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
  StoredMessage m;
  m.conversationId = st.conversationId;
  m.turnId = st.turnId;
  m.role = "tool";
  m.content = truncate(json::compact(o.result), 16000);
  m.toolCallId = call.id.empty() ? "call" : call.id;
  m.toolName = call.name;
  co_await insertMessage(tx, std::move(m));
  AuditEntry a;
  a.userId = r.userId;
  a.conversationId = st.conversationId;
  a.turnId = st.turnId;
  a.messageId = assistantId;
  const auto& names = agentNameMap();
  const auto n = names.find(call.name);
  a.tool = n == names.end() ? call.name : n->second;
  try {
    a.args = json::parse(call.arguments.empty() ? "{}" : call.arguments);
  } catch (const std::exception&) {
    a.args["_raw"] = truncate(call.arguments, 500);
  }
  if (!a.args.isObject()) a.args = Json::Value(Json::objectValue);
  a.decision = o.decision;
  a.outcome = o.outcome;
  a.approvalId = o.approvalId;
  a.error = o.error;
  if (o.outcome == "executed") {
    const auto& v = o.result;
    a.resultSummary = v.isObject() && v["id"].isString() ? "id " + v["id"].asString() : truncate(json::compact(v), 200);
  }
  co_await insertAudit(tx, a);
  co_await tx.commit();
}

// ------------------------------------------------------------------------------------------------ the loop

drogon::Task<void> agentLoop(const TurnRequest& r, TurnState& st) {
  int calls = 0;
  std::vector<std::string> executed;
  std::string replyId;
  for (int step = 1; step <= kMaxSteps; ++step) {
    if (r.channel == "telegram" && !st.proactive && telegram::enabled()) {  // "typing…" lasts about 5 s
      auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
      const auto chat = co_await telegram::chatOf(tx, r.userId);
      co_await tx.commit();
      if (chat) co_await telegram::typing(*chat);
    }
    auto req = co_await buildRequest(r, st);
    HubSink sink;
    sink.userId = r.userId;
    sink.turnId = st.turnId;
    llm::LlmResponse resp;
    std::optional<llm::LlmError> err;
    try {
      resp = co_await llm::gateway().complete(std::move(req), &sink);
    } catch (const llm::LlmError& e) {
      err = e;
    }
    if (err) {
      if (err->cls == llm::ErrorClass::ProviderUnavailable) {
        const auto mode = llm::gateway().mode();
        const auto resets = co_await localText(r, err->resetsAt);
        const auto id = co_await say(r, st.conversationId, st.turnId, offlineReply(mode, resets), "deterministic");
        co_await finishTurn(r, st.turnId, "manual", "", step - 1, calls, id);
      } else if (st.proactive) {  // nobody is waiting for it: fail quietly (the turn row says why)
        co_await finishTurn(r, st.turnId, "failed", llm::toString(err->cls), step - 1, calls, "");
      } else {
        const auto id = co_await say(
            r, st.conversationId, st.turnId,
            std::string("Sorry, I couldn't finish that (") + llm::toString(err->cls) +
                "). Your message is saved; please try again in a moment.",
            "error");
        co_await finishTurn(r, st.turnId, "failed", llm::toString(err->cls), step - 1, calls, id);
      }
      co_return;
    }

    // Persist the assistant step. A proactive turn's final message passes the Notifier gate first: delivered with
    // a notification, or kept as suppressed so the agent knows what it chose (or was not allowed) to send.
    std::string assistantId;
    std::string kind = "normal";
    {
      auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::Agent));
      StoredMessage m;
      m.conversationId = st.conversationId;
      m.turnId = st.turnId;
      m.role = "assistant";
      m.content = resp.content;
      m.toolCalls = resp.toolCalls;
      for (auto& c : m.toolCalls) {
        if (c.id.empty()) c.id = "call_" + ids::newUuidV7().substr(24);
      }
      resp.toolCalls = m.toolCalls;
      m.promptId = agentSystemPrompt().id;
      m.promptVersion = agentSystemPrompt().version + "+" + agentSystemPrompt().hash;
      m.contractVersion = tools::embeddedManifest().contractVersion;
      m.tokensIn = resp.usage.tokensIn;
      m.tokensOut = resp.usage.tokensOut;
      bool deliver = false;
      if (resp.toolCalls.empty() && st.proactive) {
        if (st.noMessage || st.silent || resp.content.empty()) {
          kind = "suppressed";
        } else {
          const auto g = co_await proactive::gate(tx, r.userId, st.urgent);
          deliver = g.deliver;
          st.gateReason = g.reason;
          if (!deliver) kind = "suppressed";
        }
      }
      m.kind = kind;
      assistantId = co_await insertMessage(tx, std::move(m));
      if (deliver) {
        proactive::Notice n;
        n.userId = r.userId;
        n.kind = "agent." + r.triggerKind;
        n.urgency = st.urgent ? "critical" : "normal";
        n.title = "TheLife";
        n.body = resp.content;
        n.conversationId = st.conversationId;
        n.messageId = assistantId;
        n.proactive = true;
        n.telegramSent = r.channel == "telegram";
        co_await proactive::notify(tx, n);
      }
      co_await tx.commit();
    }
    if (resp.toolCalls.empty()) {
      replyId = assistantId;
      if (kind == "normal") {
        Json::Value d;
        d["turn_id"] = st.turnId;
        d["message_id"] = assistantId;
        d["kind"] = "normal";
        d["text"] = resp.content;
        EventHub::instance().publish(r.userId, "message", d);
        co_await toTelegram(r, resp.content);
      }
      co_await finishTurn(r, st.turnId, "done", st.gateReason, step, calls, replyId);
      co_return;
    }

    // Reads could run in parallel; for now every call runs in order (writes must anyway).
    for (const auto& call : resp.toolCalls) {
      Json::Value started;
      started["turn_id"] = st.turnId;
      started["tool"] = call.name;
      EventHub::instance().publish(r.userId, "tool.started", started);
      CallOutcome o;
      if (++calls > kMaxToolCalls) {
        o.decision = "invalid";
        o.outcome = "failed";
        o.error = "tool call limit for this turn reached";
        o.result["error"] = *o.error;
      } else {
        o = co_await execute(r, st, call);
      }
      if (o.outcome == "executed" || o.outcome == "pending") executed.push_back(call.name + " (" + o.outcome + ")");
      co_await recordStep(r, st, assistantId, call, o);
      Json::Value finished = started;
      finished["outcome"] = o.outcome;
      EventHub::instance().publish(r.userId, "tool.finished", finished);
    }
    if (st.noMessage && st.proactive) {  // nothing worth sending: end without another model call
      co_await finishTurn(r, st.turnId, "done", "no_message", step, calls, "");
      co_return;
    }
  }

  // Step limit reached: end with a plain summary of what was done.
  std::string text = "I stopped after " + std::to_string(kMaxSteps) + " steps without finishing.";
  if (!executed.empty()) {
    text += " Done so far:";
    for (const auto& e : executed) text += "\n- " + e;
  }
  if (!st.proactive) replyId = co_await say(r, st.conversationId, st.turnId, text, "normal");
  co_await finishTurn(r, st.turnId, "done", "step limit", kMaxSteps, calls, replyId);
}

}  // namespace

std::string offlineReply(const std::string& mode, const std::string& resetsLocal) {
  if (mode == "unconfigured") {
    return "The assistant model isn't set up on this server, so I can only run commands. Send /help to see them.";
  }
  std::string s = "The assistant is offline";
  if (!resetsLocal.empty()) s += " until " + resetsLocal + " (model quota used up)";
  return s + ". Your message is saved. Commands still work: /today, /done N, /approvals, /approve N (/help lists them).";
}

void submitTurn(TurnRequest r) {
  const std::string key = r.userId + "|" + r.channel;
  {
    std::lock_guard lock(lanesMu);
    auto& lane = lanes[key];
    lane.queue.push_back(std::move(r));
    if (lane.running) return;
    lane.running = true;
  }
  drogon::async_run([key] { return drain(key); });
}

drogon::Task<void> runTurn(TurnRequest r) {
  TurnState st;
  std::string userText;
  NowInfo now;
  {
    auto tx = co_await db::TenantTx::begin(principalOf(r, PrincipalKind::User));
    now = co_await loadNow(tx, r.userId);
    if (r.trigger == "user_message" || r.trigger == "deferred_reply") {
      const auto t = co_await tx.exec(
          "SELECT t.conversation_id::text AS conv, t.message_id::text AS msg, t.status, m.content, "
          "  to_char(m.created_at AT TIME ZONE u.timezone, 'Dy HH24:MI') AS sent, "
          "  EXISTS (SELECT 1 FROM turns o WHERE o.conversation_id = t.conversation_id AND o.id <> t.id "
          "          AND o.status = 'done' AND o.seen_until >= m.created_at) AS answered "
          "FROM turns t JOIN messages m ON m.id = t.message_id JOIN users u ON u.id = t.user_id WHERE t.id = $1::uuid",
          r.turnId);
      if (t.empty() || t[0]["status"].as<std::string>() != "queued") co_return;
      st.turnId = r.turnId;
      st.conversationId = t[0]["conv"].as<std::string>();
      st.userMessageId = t[0]["msg"].as<std::string>();
      userText = t[0]["content"].as<std::string>();
      if (t[0]["answered"].as<bool>()) {  // a later turn already saw this message
        co_await tx.commit();
        co_await finishTurn(r, st.turnId, "merged", "", 0, 0, "");
        co_return;
      }
      if (r.trigger == "deferred_reply") {
        st.approvalNote = "Event (not from the user): the user's last message was sent at " +
                          t[0]["sent"].as<std::string>() +
                          " while you were offline; they were told it was saved. Answer it now, and record anything "
                          "it reports as usual.";
      }
    } else if (r.trigger == "proactive") {
      if (r.turnId.empty()) {
        const auto conv = co_await ensureConversation(tx, r.userId, r.channel);
        st.conversationId = conv.id;
        st.turnId = ids::newUuidV7();
        co_await tx.exec(
            "INSERT INTO turns (id, tenant_id, user_id, conversation_id, trigger, trigger_kind) VALUES ($1::uuid, "
            "$2::uuid, $3::uuid, $4::uuid, 'proactive', $5)",
            st.turnId, r.tenantId, r.userId, st.conversationId, r.triggerKind);
      } else {  // recorded by proactive.tick
        const auto t = co_await tx.exec(
            "SELECT conversation_id::text AS conv, status FROM turns WHERE id = $1::uuid", r.turnId);
        if (t.empty() || t[0]["status"].as<std::string>() != "queued") co_return;
        st.turnId = r.turnId;
        st.conversationId = t[0]["conv"].as<std::string>();
      }
      std::string file = r.triggerKind;
      for (auto& c : file) {
        if (c == '.') c = '_';
      }
      st.approvalNote = prompt("agent/triggers/" + file).render({{"details", r.details}});
      st.proactive = true;
      st.silent = r.silent;
      st.urgent = r.urgent;
      if (r.triggerKind == "dispute_review") st.groups.insert("detections");
    } else {
      const auto a = co_await tx.exec(
          "SELECT a.tool, a.status, a.summary, coalesce(a.error, '') AS error, coalesce(a.result::text, '') AS result, "
          "  coalesce(a.decision_reason, '') AS reason FROM approvals a WHERE a.id = $1::uuid",
          r.approvalId);
      if (a.empty()) co_return;
      const auto conv = co_await ensureConversation(tx, r.userId, r.channel);
      st.conversationId = conv.id;
      st.turnId = ids::newUuidV7();
      co_await tx.exec(
          "INSERT INTO turns (id, tenant_id, user_id, conversation_id, trigger) VALUES ($1::uuid, $2::uuid, $3::uuid, "
          "$4::uuid, 'approval')",
          st.turnId, r.tenantId, r.userId, st.conversationId);
      const auto status = a[0]["status"].as<std::string>();
      st.approvalNote = "Event (not from the user): approval " + r.approvalId + " for " +
                        a[0]["tool"].as<std::string>() + " is now '" + status + "'.";
      if (!a[0]["reason"].as<std::string>().empty()) st.approvalNote += " Reason given: " + a[0]["reason"].as<std::string>() + ".";
      if (status == "approved") st.approvalNote += " Result: " + truncate(a[0]["result"].as<std::string>(), 1500);
      if (status == "stale") {
        st.approvalNote += " The item changed before approval, so nothing ran. Re-read it and propose again only if "
                           "the change still makes sense.";
      }
      if (status == "failed") st.approvalNote += " It failed: " + a[0]["error"].as<std::string>();
      st.approvalNote += " Tell the user in one or two sentences.";
    }
    co_await tx.exec("UPDATE turns SET status = 'running', started_at = now(), updated_at = now() WHERE id = $1::uuid",
                     st.turnId);
    co_await tx.commit();
  }
  Json::Value started;
  started["turn_id"] = st.turnId;
  started["conversation_id"] = st.conversationId;
  EventHub::instance().publish(r.userId, "turn.started", started);

  // Deterministic paths: commands always, everything while no model is usable (§3.4).
  if (r.trigger == "user_message" && !userText.empty() && userText.front() == '/') {
    const auto reply = co_await runCommand(principalOf(r, PrincipalKind::User), userText, now);
    const auto id = co_await say(r, st.conversationId, st.turnId, reply, "deterministic");
    co_await finishTurn(r, st.turnId, "manual", "", 0, 0, id);
    co_return;
  }
  std::optional<llm::Clock::time_point> resetsAt;
  const auto mode = llm::gateway().mode(&resetsAt);
  if (mode != "online") {
    std::string id;
    if (r.trigger == "user_message") {  // proactive turns simply do not happen; replies wait for the reset
      const auto resets = co_await localText(r, resetsAt);
      id = co_await say(r, st.conversationId, st.turnId, offlineReply(mode, resets), "deterministic");
    }
    co_await finishTurn(r, st.turnId, "manual", "", 0, 0, id);
    co_return;
  }
  co_await agentLoop(r, st);
}

}  // namespace thelife::agent
