#include "llm/provider.h"

#include <json/reader.h>
#include <json/writer.h>

#include "core/json.h"
#include "llm/classifier.h"

namespace thelife::llm {

namespace {

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

Json::Value messageJson(const Message& m) {
  Json::Value v(Json::objectValue);
  v["role"] = m.role;
  if (!m.imageDataUris.empty()) {
    Json::Value parts(Json::arrayValue);
    if (!m.content.empty()) {
      Json::Value t;
      t["type"] = "text";
      t["text"] = m.content;
      parts.append(t);
    }
    for (const auto& uri : m.imageDataUris) {
      Json::Value p;
      p["type"] = "image_url";
      p["image_url"]["url"] = uri;
      parts.append(p);
    }
    v["content"] = parts;
  } else {
    v["content"] = m.content;
  }
  if (!m.toolCalls.empty()) {
    Json::Value calls(Json::arrayValue);
    for (const auto& c : m.toolCalls) {
      Json::Value j;
      j["id"] = c.id;
      j["type"] = "function";
      j["function"]["name"] = c.name;
      j["function"]["arguments"] = c.arguments;
      calls.append(j);
    }
    v["tool_calls"] = calls;
  }
  if (m.role == "tool") v["tool_call_id"] = m.toolCallId;
  return v;
}

std::vector<ToolCallOut> parseToolCalls(const Json::Value& arr) {
  std::vector<ToolCallOut> out;
  if (!arr.isArray()) return out;
  for (const auto& c : arr) {
    ToolCallOut t;
    t.id = c["id"].asString();
    t.name = c["function"]["name"].asString();
    const auto& args = c["function"]["arguments"];
    t.arguments = args.isString() ? args.asString() : json::compact(args);  // some servers send an object
    out.push_back(std::move(t));
  }
  return out;
}

class OpenAiProvider final : public Provider {
 public:
  OpenAiProvider(ProviderConfig cfg, std::shared_ptr<Transport> t) : cfg_(std::move(cfg)), transport_(std::move(t)) {}
  const ProviderConfig& config() const override { return cfg_; }

  LlmResponse complete(const LlmRequest& req, StreamSink* sink) override {
    const std::string model = req.vision ? cfg_.visionModel : cfg_.chatModel;
    const bool stream = sink != nullptr;
    const std::string body = json::compact(buildChatBody(req, model, stream));
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}, {"Accept", stream ? "text/event-stream" : "application/json"}};
    if (!cfg_.apiKey.empty()) headers["Authorization"] = "Bearer " + cfg_.apiKey;

    SseAccumulator acc(sink);
    std::function<void(std::string_view)> onChunk;
    if (stream) onChunk = [&acc](std::string_view b) { acc.feed(b); };
    const auto r = transport_->post(cfg_.baseUrl + "/chat/completions", headers, body, cfg_.timeoutSec,
                                    cfg_.idleTimeoutSec, onChunk);
    if (r.status < 200 || r.status >= 300) throw classify(r, Clock::now());

    LlmResponse out;
    if (stream) {
      out = acc.finish();
    } else {
      Json::Value v;
      try {
        v = json::parse(r.body);
      } catch (const std::exception&) {
        throw LlmError(ErrorClass::InvalidOutput, "response body is not JSON", r.status);
      }
      out = parseChatResponse(v);
    }
    out.provider = cfg_.name;
    if (out.model.empty()) out.model = model;
    if (auto bad = checkCompletion(out)) throw *bad;
    return out;
  }

 private:
  ProviderConfig cfg_;
  std::shared_ptr<Transport> transport_;
};

class StubProvider final : public Provider {
 public:
  explicit StubProvider(ProviderConfig cfg) : cfg_(std::move(cfg)) {
    cfg_.caps = Capabilities{false, false, false, 0};
  }
  const ProviderConfig& config() const override { return cfg_; }
  LlmResponse complete(const LlmRequest&, StreamSink*) override {
    throw LlmError(ErrorClass::ProviderUnavailable, "fallback provider is a stub");
  }

 private:
  ProviderConfig cfg_;
};

}  // namespace

std::unique_ptr<Provider> makeOpenAiProvider(ProviderConfig cfg, std::shared_ptr<Transport> transport) {
  return std::make_unique<OpenAiProvider>(std::move(cfg), std::move(transport));
}

std::unique_ptr<Provider> makeStubProvider(ProviderConfig cfg) { return std::make_unique<StubProvider>(std::move(cfg)); }

Json::Value buildChatBody(const LlmRequest& req, const std::string& model, bool stream) {
  Json::Value v(Json::objectValue);
  v["model"] = model;
  Json::Value msgs(Json::arrayValue);
  for (const auto& m : req.messages) msgs.append(messageJson(m));
  v["messages"] = msgs;
  if (!req.tools.empty()) {
    Json::Value tools(Json::arrayValue);
    for (const auto& t : req.tools) {
      Json::Value j;
      j["type"] = "function";
      j["function"]["name"] = t.name;
      j["function"]["description"] = t.description;
      j["function"]["parameters"] = t.parameters;
      tools.append(j);
    }
    v["tools"] = tools;
    v["tool_choice"] = req.toolChoice;
  }
  v["max_tokens"] = req.maxTokens;
  if (req.temperature) v["temperature"] = *req.temperature;
  // "none" leaks reasoning into content on the target model; never send it.
  if (req.reasoningEffort && *req.reasoningEffort != "none") v["reasoning_effort"] = *req.reasoningEffort;
  if (stream) {
    v["stream"] = true;
    v["stream_options"]["include_usage"] = true;
  }
  return v;
}

SplitContent splitThinking(const std::string& content) {
  const auto close = content.rfind("</think>");
  if (close == std::string::npos) return {"", content};
  std::string reasoning = content.substr(0, close);
  if (const auto open = reasoning.find("<think>"); open != std::string::npos) reasoning.erase(0, open + 7);
  return {trim(reasoning), trim(content.substr(close + 8))};
}

LlmResponse parseChatResponse(const Json::Value& body) {
  LlmResponse out;
  const auto& choices = body["choices"];
  if (!choices.isArray() || choices.empty() || !choices[0].isObject()) {
    throw LlmError(ErrorClass::InvalidOutput, "no choices in response", 200);
  }
  const auto& msg = choices[0]["message"];
  const auto split = splitThinking(msg["content"].isString() ? msg["content"].asString() : "");
  out.content = split.content;
  out.reasoning = msg["reasoning"].isString() ? msg["reasoning"].asString() : split.reasoning;
  out.toolCalls = parseToolCalls(msg["tool_calls"]);
  out.finishReason = choices[0]["finish_reason"].isString() ? choices[0]["finish_reason"].asString() : "";
  out.usage.tokensIn = body["usage"]["prompt_tokens"].asInt();
  out.usage.tokensOut = body["usage"]["completion_tokens"].asInt();
  out.model = body["model"].isString() ? body["model"].asString() : "";
  return out;
}

void SseAccumulator::feed(std::string_view bytes) {
  buf_.append(bytes);
  std::size_t pos;
  while ((pos = buf_.find('\n')) != std::string::npos) {
    std::string l = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    line(l);
  }
}

void SseAccumulator::line(std::string_view l) {
  if (l.empty()) {  // end of event
    if (!data_.empty()) event(data_);
    data_.clear();
    return;
  }
  if (l.rfind("data:", 0) == 0) {
    auto d = l.substr(5);
    if (!d.empty() && d[0] == ' ') d.remove_prefix(1);
    if (!data_.empty()) data_ += '\n';
    data_.append(d);
  }
  // comments (":") and other fields are ignored
}

void SseAccumulator::event(const std::string& data) {
  if (data == "[DONE]") {
    done_ = true;
    return;
  }
  Json::Value v;
  try {
    v = json::parse(data);
  } catch (const std::exception&) {
    streamError_ = "malformed stream event";
    return;
  }
  if (v.isMember("error")) {
    streamError_ = v["error"].isObject() && v["error"]["message"].isString() ? "stream error event" : "stream error";
    return;
  }
  if (v["model"].isString()) out_.model = v["model"].asString();
  if (v["usage"].isObject()) {
    out_.usage.tokensIn = v["usage"]["prompt_tokens"].asInt();
    out_.usage.tokensOut = v["usage"]["completion_tokens"].asInt();
  }
  const auto& choices = v["choices"];
  if (!choices.isArray() || choices.empty()) return;
  const auto& c = choices[0];
  const auto& d = c["delta"];
  if (d["reasoning"].isString()) out_.reasoning += d["reasoning"].asString();
  if (d["content"].isString() && !d["content"].asString().empty()) {
    const auto text = d["content"].asString();
    out_.content += text;
    if (sink_) {
      if (!thinkClosed_ && out_.content.find("</think>") != std::string::npos) {
        // Everything so far was leaked reasoning: retract it and show only what follows the tag.
        thinkClosed_ = true;
        sink_->reset();
        // Only leading whitespace is dropped: trailing spaces belong to the next delta's word boundary.
        std::string rest = out_.content.substr(out_.content.rfind("</think>") + 8);
        const auto first = rest.find_first_not_of(" \t\r\n");
        rest.erase(0, first == std::string::npos ? rest.size() : first);
        if (!rest.empty()) sink_->delta(rest);
      } else {
        sink_->delta(text);
      }
    }
  }
  if (d["tool_calls"].isArray()) {
    for (const auto& tc : d["tool_calls"]) {
      const auto idx = static_cast<std::size_t>(tc["index"].isIntegral() ? tc["index"].asUInt() : calls_.size());
      if (idx > 128) continue;
      if (calls_.size() <= idx) calls_.resize(idx + 1);
      auto& call = calls_[idx];
      if (tc["id"].isString()) call.id = tc["id"].asString();
      if (tc["function"]["name"].isString()) call.name += tc["function"]["name"].asString();
      const auto& args = tc["function"]["arguments"];
      if (args.isString()) call.arguments += args.asString();
      else if (args.isObject()) call.arguments = json::compact(args);
    }
  }
  if (c["finish_reason"].isString()) {
    out_.finishReason = c["finish_reason"].asString();
  }
}

LlmResponse SseAccumulator::finish() {
  if (!data_.empty()) event(data_);
  data_.clear();
  if (!streamError_.empty()) throw LlmError(ErrorClass::Transient, streamError_, 200);
  if (out_.finishReason.empty() && !done_) throw LlmError(ErrorClass::Transient, "stream ended early", 200);
  const auto split = splitThinking(out_.content);
  out_.content = split.content;
  if (out_.reasoning.empty()) out_.reasoning = split.reasoning;
  for (auto& c : calls_) {
    if (!c.name.empty()) out_.toolCalls.push_back(std::move(c));
  }
  return out_;
}

}  // namespace thelife::llm
