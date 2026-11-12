find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_library(thelife_test_support STATIC support/server_process.cc support/http.cc support/fixtures.cc
            support/api_client.cc)
target_include_directories(thelife_test_support PUBLIC .)
target_link_libraries(thelife_test_support PUBLIC thelife_core Drogon::Drogon GTest::gtest PRIVATE thelife_warnings)
target_compile_definitions(thelife_test_support PUBLIC
  THELIFE_BACKEND_BIN="$<TARGET_FILE:thelife-backend>")

# unit: no database, no server
add_executable(thelife_unit_tests unit/core_test.cc unit/tools_test.cc)
target_link_libraries(thelife_unit_tests PRIVATE thelife_core GTest::gtest_main thelife_warnings)
gtest_discover_tests(thelife_unit_tests PROPERTIES LABELS "unit")

# api: real binary on a free port (+ database when THELIFE_TEST_DB_URL / THELIFE_TEST_ADMIN_DB_URL are set)
add_executable(thelife_api_tests api/health_test.cc api/auth_test.cc api/planning_test.cc)
target_link_libraries(thelife_api_tests PRIVATE thelife_test_support GTest::gtest_main thelife_warnings)
add_dependencies(thelife_api_tests thelife-backend)
gtest_discover_tests(thelife_api_tests PROPERTIES LABELS "api")

# isolation: RLS catalog, leak and privilege checks directly against Postgres
add_executable(thelife_isolation_tests isolation/rls_test.cc)
target_link_libraries(thelife_isolation_tests PRIVATE thelife_test_support GTest::gtest_main thelife_warnings)
gtest_discover_tests(thelife_isolation_tests PROPERTIES LABELS "isolation")
