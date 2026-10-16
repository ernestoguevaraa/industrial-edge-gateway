cmake_minimum_required(VERSION 3.25)

project(
  industrial_edge_gateway
  VERSION 0.1.0
  DESCRIPTION "C++20 Industrial IoT edge gateway (OPC UA -> MQTT)"
  LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GATEWAY_BUILD_TESTS "Build unit tests" ON)
option(GATEWAY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(GATEWAY_ENABLE_CLANG_TIDY "Run clang-tidy on project sources during the build" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Dependencies)

if(GATEWAY_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 REQUIRED)
  # Set after dependencies are declared so third-party targets are not analysed.
  set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
endif()

if(GATEWAY_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(gateway)
