#include "http/rest_adapter.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "core/errors.h"
#include "core/json.h"
#include "http/route.h"
#include "tools/registry.h"

namespace thelife::http {

namespace {

using tools::ToolDef;

drogon::HttpMethod methodOf(const std::string& m) {
  if (m == "GET") return drogon::Get;
  if (m == "POST") return drogon::Post;
  if (m == "PATCH") return drogon::Patch;
  if (m == "PUT") return drogon::Put;
  if (m == "DELETE") return drogon::Delete;
  throw std::runtime_error("unsupported method " + m);
}

std::string schemaType(const Json::Value& prop) {
  const auto& t = prop["type"];
  if (t.isString()) return t.asString();
  for (const auto& x : t) {
    if (x.asString() != "null") return x.asString();
  }
  return "string";
}

// Query strings carry text; coerce to the schema type so validation sees real types.
Json::Value coerceQuery(const std::string& name, const std::string& raw, const Json::Value& prop) {
  const auto type = schemaType(prop);
  if (type == "integer") {
    char* end = nullptr;
    const long v = std::strtol(raw.c_str(), &end, 10);
    if (raw.empty() || *end != '\0') throw ApiError::validation({{name, "must be an integer"}});
    return Json::Value(static_cast<Json::Int64>(v));
  }
  if (type == "number") {
    char* end = nullptr;
    const double v = std::strtod(raw.c_str(), &end);
    if (raw.empty() || *end != '\0') throw ApiError::validation({{name, "must be a number"}});
    return Json::Value(v);
  }
  if (type == "boolean") {
    if (raw == "true") return Json::Value(true);
    if (raw == "false") return Json::Value(false);
    throw ApiError::validation({{name, "must be true or false"}});
  }
  return Json::Value(raw);
}

std::optional<int> parseIfMatch(const std::string& header) {
  // "3" (quoted strong ETag); weak or list forms are not accepted for writes.
  if (header.size() < 3 || header.front() != '"' || header.back() != '"') return std::nullopt;
  const auto digits = header.substr(1, header.size() - 2);
  if (digits.empty() || digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  return std::stoi(digits);
}

std::vector<auth::PrincipalKind> kindsFor(const ToolDef& def) {
  std::vector<auth::PrincipalKind> out;
  if (def.principals.count("user")) out.push_back(auth::PrincipalKind::User);
  if (def.principals.count("api_key")) out.push_back(auth::PrincipalKind::ApiKey);
  if (def.principals.count("device")) out.push_back(auth::PrincipalKind::Device);
  return out;  // agents never call over HTTP
}

void registerOne(const ToolDef& toolDef) {
  const std::string name = toolDef.name;
  addRoute(
      toolDef.rest->path, methodOf(toolDef.rest->method), Access::Authenticated,
      [name](RequestContext& ctx) -> drogon::Task<drogon::HttpResponsePtr> {
        const auto& def = tools::registry().tool(name);
        const auto& rest = *def.rest;
        Json::Value args(Json::objectValue);

        // path
        const auto& params = ctx.req->getRoutingParameters();
        for (size_t i = 0; i < rest.pathArgs.size() && i < params.size(); ++i) args[rest.pathArgs[i]] = params[i];

        // query
        // A parameter that is present but empty (?to=) is a value to validate, not an absent one.
        const auto& props = def.inputSchema["properties"];
        const auto& query = ctx.req->getParameters();
        for (const auto& q : rest.queryArgs) {
          const auto it = query.find(q);
          if (it != query.end()) args[q] = coerceQuery(q, it->second, props[q]);
        }

        // body
        const bool hasBody = !rest.bodyArgs.empty() || rest.method == "POST" || rest.method == "PATCH";
        if (hasBody && !(rest.method == "POST" && rest.bodyArgs.empty() && ctx.req->body().empty())) {
          Json::Value body;
          if (rest.method == "PATCH") {
            body = ctx.jsonBody({"application/merge-patch+json", "application/json"});
          } else if (!rest.contentType.empty() && rest.contentType != "application/json") {
            throw ApiError{415, "unsupported-media-type", "Unsupported media type",
                           rest.contentType + " is not supported yet."};
          } else {
            body = ctx.jsonBody();
          }
          for (const auto& k : body.getMemberNames()) {
            if (args.isMember(k)) throw ApiError::validation({{"/" + k, "conflicts with a path or query argument"}});
            args[k] = body[k];
          }
        }

        // preconditions
        if (rest.ifMatch) {
          const auto& h = ctx.req->getHeader("if-match");
          if (h.empty()) throw ApiError::preconditionRequired();
          const auto v = parseIfMatch(h);
          if (!v) throw ApiError::validation({{"If-Match", "must be a quoted version, for example \"3\""}});
          args["expected_version"] = *v;
        }
        tools::InvokeOptions opts;
        opts.requestId = ctx.requestId;
        if (rest.method == "POST") {
          // Present but empty is invalid, not absent. Visible ASCII only, so length is unambiguous.
          const auto& headers = ctx.req->headers();
          if (const auto h = headers.find("idempotency-key"); h != headers.end()) {
            const auto& key = h->second;
            const bool visible = std::all_of(key.begin(), key.end(), [](char c) { return c > ' ' && c < 127; });
            if (key.size() < 8 || key.size() > 128 || !visible) {
              throw ApiError::validation({{"Idempotency-Key", "must be 8 to 128 visible ASCII characters"}});
            }
            opts.idempotencyKey = key;
          }
        }

        // Long poll (`wait` seconds, for example the collector's notification fetch): repeat the read in short
        // transactions until it returns items or the wait is over, so no connection idles in a transaction.
        const int waitSec = def.effect == tools::Effect::Read && args.isMember("wait") ? args["wait"].asInt() : 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::clamp(waitSec, 0, 55));
        auto result = co_await tools::registry().invoke(name, ctx.who(), args, opts);
        while (waitSec > 0 && result.value["items"].isArray() && result.value["items"].empty()) {
          const auto left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
          if (left <= 0.05) break;
          co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), std::min(1.0, left));
          result = co_await tools::registry().invoke(name, ctx.who(), args, opts);
        }

        drogon::HttpResponsePtr resp;
        if (result.outcome != tools::Outcome::Executed) {
          resp = jsonResponse(result.value, drogon::k202Accepted);
        } else if (result.status == 204 || result.value.isNull()) {
          resp = noContent();
          if (result.status != 204) resp->setStatusCode(static_cast<drogon::HttpStatusCode>(result.status));
        } else {
          resp = jsonResponse(result.value, static_cast<drogon::HttpStatusCode>(result.status));
          if (result.value.isObject() && result.value["version"].isInt()) {
            resp->addHeader("ETag", "\"" + std::to_string(result.value["version"].asInt()) + "\"");
          }
        }
        if (result.replayed) resp->addHeader("Idempotent-Replayed", "true");
        co_return resp;
      },
      kindsFor(toolDef));
}

}  // namespace

void registerToolRoutes() {
  auto& reg = tools::registry();
  for (const auto& [name, def] : reg.manifest().tools) {
    if (def.rest && def.exposure.count("rest") && reg.isBound(name)) registerOne(def);
  }
}

}  // namespace thelife::http
