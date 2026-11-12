find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_library(thelife_test_support STATIC support/server_process.cc support/http.cc)
target_include_directories(thelife_test_support PUBLIC .)
target_link_libraries(thelife_test_support PUBLIC Drogon::Drogon GTest::gtest PRIVATE thelife_warnings)
target_compile_definitions(thelife_test_support PUBLIC
  THELIFE_BACKEND_BIN="$<TARGET_FILE:thelife-backend>")

add_executable(thelife_api_tests api/health_test.cc)
target_link_libraries(thelife_api_tests PRIVATE thelife_test_support thelife_core GTest::gtest_main thelife_warnings)
add_dependencies(thelife_api_tests thelife-backend)
# Tests that need Postgres read THELIFE_TEST_DB_URL and skip themselves when it is unset.
gtest_discover_tests(thelife_api_tests PROPERTIES LABELS "api")
