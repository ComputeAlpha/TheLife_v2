#include "http/health.h"

#include <drogon/drogon.h>

#include "core/build_info.h"
#include "db/database.h"

namespace thelife::http {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

void registerHealthRoutes() {
  drogon::app().registerHandler(
      "/healthz",
      [](HttpRequestPtr) -> drogon::Task<HttpResponsePtr> {
        Json::Value body;
        body["status"] = "ok";
        body["version"] = build::kVersion;
        co_return HttpResponse::newHttpJsonResponse(body);
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/readyz",
      [](HttpRequestPtr) -> drogon::Task<HttpResponsePtr> {
        const auto ready = co_await db::Database::instance().checkReady(build::kSchemaVersion);
        Json::Value body;
        body["status"] = ready.ok ? "ready" : "not_ready";
        body["checks"]["database"] = ready.detail;
        body["schema_version"] = build::kSchemaVersion;
        auto resp = HttpResponse::newHttpJsonResponse(body);
        if (!ready.ok) resp->setStatusCode(drogon::k503ServiceUnavailable);
        co_return resp;
      },
      {drogon::Get});
}

}  // namespace thelife::http
