find_package(Drogon CONFIG REQUIRED)

option(THELIFE_WERROR "Treat warnings as errors" OFF)

# Latest migration version (numeric filename prefix). /readyz requires the DB to be at this version.
file(GLOB _migrations RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/migrations" CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/migrations/*.sql")
list(SORT _migrations)
list(GET _migrations -1 _latest_migration)
string(REGEX MATCH "^[0-9]+" THELIFE_SCHEMA_VERSION "${_latest_migration}")
message(STATUS "TheLife schema version: ${THELIFE_SCHEMA_VERSION}")

configure_file(src/core/build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/core/build_info.h" @ONLY)

add_library(thelife_warnings INTERFACE)
target_compile_options(thelife_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                       $<$<BOOL:${THELIFE_WERROR}>:-Werror>)

add_library(thelife_core STATIC
  src/core/env.cc
  src/db/database.cc
  src/http/health.cc
)
target_include_directories(thelife_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(thelife_core PUBLIC Drogon::Drogon PRIVATE thelife_warnings)

add_executable(thelife-backend src/main.cc)
target_link_libraries(thelife-backend PRIVATE thelife_core thelife_warnings)

if(THELIFE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
