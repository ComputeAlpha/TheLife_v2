# TheLife collector (docs/collector_lld.md). X11/xcb and libsystemd come from the system (pkg-config), the rest
# from the vcpkg manifest at the repository root.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-randr xcb-screensaver xcb-shm xcb-res)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(CURL CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

set(THELIFE_COLLECTOR_VERSION "${PROJECT_VERSION}")

add_library(thelife_collector_warnings INTERFACE)
target_compile_options(thelife_collector_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                       -Wno-sign-conversion $<$<BOOL:${THELIFE_WERROR}>:-Werror>)

add_library(thelife_collector_core STATIC
  src/core/time.cc
  src/core/json.cc
  src/core/log.cc
  src/core/files.cc
  src/config/local_config.cc
  src/config/server_config.cc
  src/config/token_store.cc
  src/queue/queue.cc
  src/net/http.cc
  src/net/uploader.cc
  src/x11/xcb_source.cc
  src/probes/window_probe.cc
  src/probes/idle_probe.cc
  src/probes/proc_probe.cc
  src/probes/lock_probe.cc
  src/capture/image.cc
  src/sched/scheduler.cc
  src/notify/notifications.cc
  src/notify/desktop.cc
  src/app/state.cc
  src/app/daemon.cc)
target_include_directories(thelife_collector_core PUBLIC src)
target_compile_definitions(thelife_collector_core PUBLIC THELIFE_COLLECTOR_VERSION="${THELIFE_COLLECTOR_VERSION}")
target_link_libraries(thelife_collector_core
  PUBLIC JsonCpp::JsonCpp
  PRIVATE PkgConfig::XCB PkgConfig::SYSTEMD CURL::libcurl unofficial::sqlite3::sqlite3
          libjpeg-turbo::turbojpeg-static tomlplusplus::tomlplusplus thelife_collector_warnings)

add_executable(thelife-collector src/main.cc)
target_link_libraries(thelife-collector PRIVATE thelife_collector_core thelife_collector_warnings)

install(TARGETS thelife-collector RUNTIME DESTINATION bin)
install(FILES packaging/systemd/thelife-collector.service DESTINATION lib/systemd/user)

if(THELIFE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
