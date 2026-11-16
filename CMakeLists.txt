cmake_minimum_required(VERSION 3.28)
project(thelife VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(THELIFE_BUILD_TESTS "Build tests" ON)
option(THELIFE_BUILD_BACKEND "Build backend" ON)
# The collector needs the X11/xcb and libsystemd development packages from the system (collector/README.md).
option(THELIFE_BUILD_COLLECTOR "Build the X11 collector" ON)

if(THELIFE_BUILD_TESTS)
  enable_testing()
endif()

if(THELIFE_BUILD_BACKEND)
  add_subdirectory(backend)
endif()

if(THELIFE_BUILD_COLLECTOR)
  add_subdirectory(collector)
endif()
