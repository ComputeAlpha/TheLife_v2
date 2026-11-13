#include <drogon/drogon.h>

#include <exception>
#include <iostream>

#include "core/build_info.h"
#include "core/env.h"
#include "db/database.h"
#include "http/auth_routes.h"
#include "http/health.h"
#include "http/llm_routes.h"
#include "http/rest_adapter.h"
#include "http/route.h"
#include "llm/gateway.h"
#include "modules/modules.h"
#include "tools/registry.h"

int main() {
  try {
    const auto addr = thelife::env::get("THELIFE_LISTEN_ADDR", "127.0.0.1");
    const auto port = thelife::env::getInt("THELIFE_LISTEN_PORT", 8080, 1, 65535);
    const auto threads = thelife::env::getInt("THELIFE_THREADS", 2, 1, 64);
    const auto dbConns = thelife::env::getInt("THELIFE_DB_CONNECTIONS", 4, 1, 64);
    const auto dbTimeout = thelife::env::getInt("THELIFE_DB_QUERY_TIMEOUT_S", 5, 1, 300);

    thelife::db::Database::init(thelife::env::get("THELIFE_DB_URL"), static_cast<std::size_t>(dbConns),
                                static_cast<double>(dbTimeout));
    thelife::http::registerHealthRoutes();
    thelife::http::registerAuthRoutes();
    thelife::llm::initGateway();
    thelife::http::registerLlmRoutes();
    auto& registry = thelife::tools::registry();
    thelife::modules::bindAll(registry);
    registry.verifyComplete(thelife::build::kStage);  // refuse to start with an unimplemented contract tool
    thelife::http::registerToolRoutes();
    thelife::http::installMethodNotAllowed();

    LOG_INFO << "thelife-backend " << thelife::build::kVersion << " listening on " << addr << ":" << port;
    drogon::app()
        .setLogLevel(thelife::env::get("THELIFE_LOG_LEVEL", "info") == "debug" ? trantor::Logger::kDebug
                                                                                 : trantor::Logger::kInfo)
        .addListener(addr, static_cast<uint16_t>(port))
        .setThreadNum(static_cast<size_t>(threads))
        .setServerHeaderField("thelife")
        .run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
