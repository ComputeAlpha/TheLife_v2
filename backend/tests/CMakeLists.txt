find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_library(thelife_test_support STATIC support/server_process.cc support/http.cc support/fixtures.cc
            support/api_client.cc
            support/fake_llm.cc)
target_include_directories(thelife_test_support PUBLIC .)
target_link_libraries(thelife_test_support PUBLIC thelife_core Drogon::Drogon GTest::gtest PRIVATE thelife_warnings)
target_compile_definitions(thelife_test_support PUBLIC
  THELIFE_BACKEND_BIN="$<TARGET_FILE:thelife-backend>"
  THELIFE_FAKE_LLM="${CMAKE_SOURCE_DIR}/docker/fake-llm/fake_llm.py"
  THELIFE_FAKE_EMBEDDER="${CMAKE_SOURCE_DIR}/docker/fake-embedder/fake_embedder.py"
  THELIFE_FAKE_TELEGRAM="${CMAKE_SOURCE_DIR}/docker/fake-telegram/fake_telegram.py")

# unit: no database, no server
add_executable(thelife_unit_tests unit/core_test.cc unit/tools_test.cc unit/recurrence_test.cc unit/llm_test.cc
               unit/scoring_test.cc unit/field_crypt_test.cc unit/ingest_test.cc)
target_link_libraries(thelife_unit_tests PRIVATE thelife_core GTest::gtest_main thelife_warnings)
target_compile_definitions(thelife_unit_tests PRIVATE
  THELIFE_LLM_FIXTURES="${CMAKE_SOURCE_DIR}/docker/fake-llm/fixtures" THELIFE_REPO_ROOT="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(thelife_unit_tests PROPERTIES LABELS "unit")

# api: real binary on a free port (+ database when THELIFE_TEST_DB_URL / THELIFE_TEST_ADMIN_DB_URL are set)
add_executable(thelife_api_tests api/health_test.cc api/auth_test.cc api/planning_test.cc
               api/resources_test.cc api/contract_test.cc api/domain_test.cc api/llm_test.cc
               api/agent_test.cc api/memory_test.cc api/web_test.cc api/embeddings_test.cc
               api/scoring_test.cc api/proactive_test.cc api/telegram_test.cc
               api/encryption_test.cc api/ingest_test.cc api/web_session_test.cc)
target_link_libraries(thelife_api_tests PRIVATE thelife_test_support GTest::gtest_main CURL::libcurl thelife_warnings)
add_dependencies(thelife_api_tests thelife-backend)
gtest_discover_tests(thelife_api_tests PROPERTIES LABELS "api")

# isolation: RLS catalog, leak and privilege checks directly against Postgres
add_executable(thelife_isolation_tests isolation/rls_test.cc)
target_link_libraries(thelife_isolation_tests PRIVATE thelife_test_support GTest::gtest_main thelife_warnings)
gtest_discover_tests(thelife_isolation_tests PROPERTIES LABELS "isolation")

# live: real LLM endpoint, skipped unless THELIFE_LIVE_LLM_URL is set (manual / nightly; spends quota)
add_executable(thelife_live_tests live/llm_live_test.cc)
target_link_libraries(thelife_live_tests PRIVATE thelife_core GTest::gtest_main thelife_warnings)
gtest_discover_tests(thelife_live_tests PROPERTIES LABELS "live")

# embedder: tokenizer parity always builds; the model test needs the ONNX Runtime build and local model files
add_executable(thelife_embedder_tests embedder/embedder_test.cc)
target_link_libraries(thelife_embedder_tests PRIVATE thelife_tokenizer JsonCpp::JsonCpp GTest::gtest_main thelife_warnings)
target_compile_definitions(thelife_embedder_tests PRIVATE THELIFE_REPO_ROOT="${CMAKE_SOURCE_DIR}")
if(TARGET thelife_embed_model)
  target_link_libraries(thelife_embedder_tests PRIVATE thelife_embed_model)
  set_target_properties(thelife_embedder_tests PROPERTIES
    BUILD_RPATH "${CMAKE_BINARY_DIR}/_deps/onnxruntime-linux-x64-gpu-1.26.0/lib")
else()
  target_compile_definitions(thelife_embedder_tests PRIVATE THELIFE_NO_ORT)
endif()
gtest_discover_tests(thelife_embedder_tests PROPERTIES LABELS "embedder")
