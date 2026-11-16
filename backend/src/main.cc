#include <drogon/drogon.h>

#include <exception>
#include <iostream>

#include "core/build_info.h"
#include "core/env.h"
#include "db/database.h"
#include "http/auth_routes.h"
#include "http/chat_routes.h"
#include "http/health.h"
#include "http/llm_routes.h"
#include "http/rest_adapter.h"
#include "http/route.h"
#include "db/field_crypt.h"
#include "ingest/jobs.h"
#include "jobs/worker.h"
#include "llm/gateway.h"
#include "memory/embedder.h"
#include "memory/jobs.h"
#include "memory/semantic.h"
#include "scoring/jobs.h"
#include "proactive/tick.h"
#include "telegram/bot.h"
#include "web/search.h"
#include "modules/modules.h"
#include "tools/registry.h"

constexpr std::size_t kMaxBody = 48 * 1024 * 1024;

int main() {
  try {
    const auto addr = thelife::env::get("THELIFE_LISTEN_ADDR", "127.0.0.1");
    const auto port = thelife::env::getInt("THELIFE_LISTEN_PORT", 8080, 1, 65535);
    const auto threads = thelife::env::getInt("THELIFE_THREADS", 2, 1, 64);
    const auto dbConns = thelife::env::getInt("THELIFE_DB_CONNECTIONS", 4, 1, 64);
    const auto dbTimeout = thelife::env::getInt("THELIFE_DB_QUERY_TIMEOUT_S", 5, 1, 300);

    thelife::db::Database::init(thelife::env::get("THELIFE_DB_URL"), static_cast<std::size_t>(dbConns),
                                static_cast<double>(dbTimeout));
    // Encrypted fields need the KEK; refuse to start without it rather than fail on the first write.
    if (thelife::db::Database::instance().configured()) thelife::fieldcrypt::init();
    thelife::http::registerHealthRoutes();
    thelife::http::registerAuthRoutes();
    thelife::llm::initGateway();
    thelife::web::init();
    thelife::http::registerLlmRoutes();
    thelife::http::registerChatRoutes();
    auto& registry = thelife::tools::registry();
    thelife::modules::bindAll(registry);
    registry.verifyComplete(thelife::build::kStage);  // refuse to start with an unimplemented contract tool
    thelife::http::registerToolRoutes();
    thelife::http::installMethodNotAllowed();
    thelife::memory::registerJobs();
    thelife::memory::registerSemanticJobs();
    thelife::scoring::registerJobs();
    // Proactive work (routines, reminders, triggers) runs unless THELIFE_PROACTIVE=off.
    if (thelife::env::get("THELIFE_PROACTIVE", "on") != "off") thelife::proactive::registerTick();
    thelife::memory::initEmbedder();
    thelife::telegram::registerJobs();
    thelife::fieldcrypt::registerJobs();
    thelife::ingest::registerJobs();
    if (thelife::db::Database::instance().configured()) {
      thelife::jobs::startWorker();
      thelife::telegram::init();  // long polling, when THELIFE_TELEGRAM_BOT_TOKEN is set
    }

    LOG_INFO << "thelife-backend " << thelife::build::kVersion << " listening on " << addr << ":" << port;
    drogon::app()
        .setLogLevel(thelife::env::get("THELIFE_LOG_LEVEL", "info") == "debug" ? trantor::Logger::kDebug
                                                                                 : trantor::Logger::kInfo)
        .addListener(addr, static_cast<uint16_t>(port))
        .setThreadNum(static_cast<size_t>(threads))
        .setServerHeaderField("thelife")
        // Request bodies stay in memory: Drogon spills bodies over the memory limit to files under the upload
        // path, and capture images (and observation titles) must never reach the disk. The total limit fits a
        // capture of 8 monitors; larger bodies are refused before they are read.
        .setClientMaxBodySize(kMaxBody)
        .setClientMaxMemoryBodySize(kMaxBody)
        .setUploadPath(thelife::env::get("THELIFE_UPLOAD_DIR", "/tmp/thelife-backend-uploads"))
        .run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
