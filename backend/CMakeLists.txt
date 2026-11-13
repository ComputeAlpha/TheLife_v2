find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(valijson CONFIG REQUIRED)
find_package(LibIcal CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

option(THELIFE_WERROR "Treat warnings as errors" OFF)

# Latest migration version (numeric filename prefix). /readyz requires the DB to be at this version.
file(GLOB _migrations RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/migrations" CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/migrations/*.sql")
list(SORT _migrations)
list(GET _migrations -1 _latest_migration)
string(REGEX MATCH "^[0-9]+" THELIFE_SCHEMA_VERSION "${_latest_migration}")
message(STATUS "TheLife schema version: ${THELIFE_SCHEMA_VERSION}")

# The tool manifest is compiled into the binary so code and contract cannot drift (docs/tool_registry.md §5).
set(THELIFE_MANIFEST "${CMAKE_SOURCE_DIR}/api/generated/tools.json")
set(THELIFE_MANIFEST_CC "${CMAKE_CURRENT_BINARY_DIR}/generated/tools_manifest.cc")
add_custom_command(
  OUTPUT "${THELIFE_MANIFEST_CC}"
  COMMAND ${CMAKE_COMMAND} -DIN=${THELIFE_MANIFEST} -DOUT=${THELIFE_MANIFEST_CC} -DSYMBOL=kToolsJson
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
  DEPENDS "${THELIFE_MANIFEST}" ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
  COMMENT "Embedding tool manifest")

# Prompts are compiled in too; their version and hash are recorded with every message and LLM call
# (agent_lld.md §9). prompts/<dir>/<name>.md becomes the symbol kPrompt_<dir>_<name>.
file(GLOB_RECURSE _prompts RELATIVE "${CMAKE_SOURCE_DIR}/prompts" CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/prompts/*.md")
set(THELIFE_PROMPT_SOURCES)
set(_prompt_table "")
foreach(_p IN LISTS _prompts)
  string(REGEX REPLACE "\\.md$" "" _stem "${_p}")
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" _sym "kPrompt_${_stem}")
  set(_out "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts/${_sym}.cc")
  add_custom_command(
    OUTPUT "${_out}"
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/prompts/${_p} -DOUT=${_out} -DSYMBOL=${_sym}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS "${CMAKE_SOURCE_DIR}/prompts/${_p}" ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
    COMMENT "Embedding prompt ${_p}")
  list(APPEND THELIFE_PROMPT_SOURCES "${_out}")
  string(APPEND _prompt_decls "extern const std::string_view ${_sym};\n")
  string(APPEND _prompt_table "  {\"${_stem}\", ${_sym}},\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts/table.cc.in"
     "// GENERATED: every prompt file under prompts/.\n#include <map>\n#include <string>\n#include <string_view>\n"
     "namespace thelife::generated {\n${_prompt_decls}const std::map<std::string, std::string_view>& promptFiles() {\n"
     "  static const std::map<std::string, std::string_view> m{\n${_prompt_table}  };\n  return m;\n}\n}\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/prompts/table.cc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts/table.cc" COPYONLY)
list(APPEND THELIFE_PROMPT_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/generated/prompts/table.cc")

set(THELIFE_STAGE 2 CACHE STRING "Delivery stage implemented by this build")
configure_file(src/core/build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/core/build_info.h" @ONLY)

add_library(thelife_warnings INTERFACE)
target_compile_options(thelife_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                       $<$<BOOL:${THELIFE_WERROR}>:-Werror>)

add_library(thelife_core STATIC
  src/core/env.cc
  src/core/ids.cc
  src/core/crypto.cc
  src/core/errors.cc
  src/core/json.cc
  src/core/blocking.cc
  src/core/recurrence.cc
  src/db/database.cc
  src/db/tenant_tx.cc
  src/db/llm_store.cc
  src/db/job_store.cc
  src/jobs/worker.cc
  src/memory/registry.cc
  src/memory/jobs.cc
  src/memory/embedder.cc
  src/memory/semantic.cc
  src/scoring/engine.cc
  src/scoring/inputs.cc
  src/scoring/jobs.cc
  src/modules/scoring.cc
  src/modules/proactive.cc
  src/modules/telegram.cc
  src/proactive/notifier.cc
  src/proactive/tick.cc
  src/telegram/bot.cc
  src/db/telegram_store.cc
  src/db/field_crypt.cc
  src/modules/memory.cc
  src/modules/web.cc
  src/web/search.cc
  src/auth/authenticator.cc
  src/auth/rate_limiter.cc
  src/http/route.cc
  src/http/health.cc
  src/http/auth_routes.cc
  src/http/rest_adapter.cc
  src/http/llm_routes.cc
  src/llm/classifier.cc
  src/llm/transport_curl.cc
  src/llm/provider.cc
  src/llm/gateway.cc
  src/llm/init.cc
  src/tools/manifest.cc
  src/tools/schema_validator.cc
  src/tools/policy.cc
  src/tools/registry.cc
  src/crud/resource.cc
  src/modules/planning.cc
  src/modules/common.cc
  src/modules/domain.cc
  src/modules/tasks.cc
  src/modules/calendar.cc
  src/modules/detections.cc
  src/modules/access.cc
  src/modules/agent.cc
  src/modules/notifications.cc
  src/agent/prompt.cc
  src/agent/events.cc
  src/agent/store.cc
  src/agent/runner.cc
  src/agent/commands.cc
  src/http/chat_routes.cc
  "${THELIFE_MANIFEST_CC}"
  ${THELIFE_PROMPT_SOURCES}
)
target_include_directories(thelife_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(thelife_core PUBLIC Drogon::Drogon unofficial-sodium::sodium valijson ical CURL::libcurl PRIVATE thelife_warnings)

add_executable(thelife-backend src/main.cc)
target_link_libraries(thelife-backend PRIVATE thelife_core thelife_warnings)

add_executable(thelife-admin tools/admin_cli/main.cc)
target_link_libraries(thelife-admin PRIVATE thelife_core thelife_warnings)

find_package(Threads REQUIRED)
add_subdirectory(embedder)

if(THELIFE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
