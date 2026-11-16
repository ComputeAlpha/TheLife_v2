find_package(GTest CONFIG REQUIRED)
find_package(valijson CONFIG REQUIRED)
include(GoogleTest)

# unit: no X server, no network
add_executable(thelife_collector_unit_tests unit/core_test.cc unit/config_test.cc unit/queue_test.cc
               unit/probes_test.cc unit/capture_test.cc unit/notify_test.cc)
target_include_directories(thelife_collector_unit_tests PRIVATE . ../src)
target_link_libraries(thelife_collector_unit_tests PRIVATE thelife_collector_core libjpeg-turbo::turbojpeg-static
                      valijson GTest::gtest_main thelife_collector_warnings)
target_compile_definitions(thelife_collector_unit_tests PRIVATE THELIFE_REPO_ROOT="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(thelife_collector_unit_tests PROPERTIES LABELS "collector;unit")

# system: the real binary and X connection under Xvfb, against tests/fixtures/fake_backend.py
pkg_check_modules(XCB_TEST REQUIRED IMPORTED_TARGET xcb xcb-xtest)
find_package(CURL CONFIG REQUIRED)
add_library(thelife_collector_test_support STATIC support/system.cc)
target_include_directories(thelife_collector_test_support PUBLIC . ../src)
target_link_libraries(thelife_collector_test_support PUBLIC thelife_collector_core PkgConfig::XCB_TEST CURL::libcurl
                      GTest::gtest PRIVATE thelife_collector_warnings)
target_compile_definitions(thelife_collector_test_support PUBLIC
  THELIFE_FAKE_BACKEND="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fake_backend.py"
  THELIFE_COLLECTOR_BIN="$<TARGET_FILE:thelife-collector>"
  THELIFE_REPO_ROOT="${CMAKE_SOURCE_DIR}")

add_executable(thelife_collector_system_tests system/x11_test.cc system/daemon_test.cc)
target_link_libraries(thelife_collector_system_tests PRIVATE thelife_collector_test_support
                      libjpeg-turbo::turbojpeg-static PkgConfig::SYSTEMD valijson GTest::gtest_main
                      thelife_collector_warnings)
add_dependencies(thelife_collector_system_tests thelife-collector)
gtest_discover_tests(thelife_collector_system_tests PROPERTIES LABELS "collector;system")

# e2e: the collector against the real backend and the test database (needs the backend build)
if(TARGET thelife_test_support)
  foreach(_e backend process)
    add_executable(thelife_collector_e2e_${_e}_tests e2e/${_e}_e2e_test.cc)
    target_link_libraries(thelife_collector_e2e_${_e}_tests PRIVATE thelife_test_support thelife_collector_test_support
                          unofficial::sqlite3::sqlite3 GTest::gtest_main thelife_collector_warnings)
    add_dependencies(thelife_collector_e2e_${_e}_tests thelife-collector thelife-backend)
  endforeach()
  gtest_discover_tests(thelife_collector_e2e_backend_tests PROPERTIES LABELS "collector;e2e")
  # About three minutes of real focus time: `ctest -LE slow` leaves it out.
  gtest_discover_tests(thelife_collector_e2e_process_tests PROPERTIES LABELS "collector;e2e;slow" TIMEOUT 400)
endif()
