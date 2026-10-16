# Third-party dependencies, pinned to release archives and verified by SHA256.
include(FetchContent)

# yaml-cpp 0.8.0 declares an old cmake_minimum_required that CMake >= 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
  spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.tar.gz
  URL_HASH SHA256=9962648c9b4f1a7bbc76fd8d9172555bad1871fdb14ff4f842ef87949682caa5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)

FetchContent_Declare(
  yaml-cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
  URL_HASH SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spdlog yaml-cpp nlohmann_json)

# open62541: the official single-file (amalgamated) release assets. They carry no
# CMakeLists.txt, so FetchContent only downloads and verifies them.
set(GATEWAY_OPEN62541_VERSION 1.4.21)
FetchContent_Declare(
  open62541_header
  URL https://github.com/open62541/open62541/releases/download/v${GATEWAY_OPEN62541_VERSION}/open62541.h
  URL_HASH SHA256=7ccf5f199136a0223ef534fbc30b9abb165ebf89e6123e2c653680d6c8cfae81
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_Declare(
  open62541_source
  URL https://github.com/open62541/open62541/releases/download/v${GATEWAY_OPEN62541_VERSION}/open62541.c
  URL_HASH SHA256=cfb8966b966af867cc3e960a000884daed02ca620fc6391e4882ec1e4bfe96ea
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(open62541_header open62541_source)

# The amalgamation fixes its feature set at release time; fail early if it lacks what we use.
set(open62541_h "${open62541_header_SOURCE_DIR}/open62541.h")
foreach(required "#define UA_OPEN62541_VERSION \"v${GATEWAY_OPEN62541_VERSION}\""
                 "#define UA_ENABLE_SUBSCRIPTIONS" "#define UA_ARCHITECTURE_POSIX")
  file(STRINGS "${open62541_h}" found REGEX "^${required}$" LIMIT_COUNT 1)
  if(NOT found)
    message(FATAL_ERROR "open62541.h lacks required configuration line: ${required}")
  endif()
endforeach()

# The release amalgamation is built with UA_ENABLE_ENCRYPTION_MBEDTLS, so it needs mbedTLS
# even though MVP 1 only uses SecurityPolicy None (Ubuntu: libmbedtls-dev).
find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h)
find_library(MBEDTLS_LIBRARY mbedtls)
find_library(MBEDX509_LIBRARY mbedx509)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto)
if(NOT MBEDTLS_INCLUDE_DIR OR NOT MBEDTLS_LIBRARY OR NOT MBEDX509_LIBRARY OR NOT MBEDCRYPTO_LIBRARY)
  message(FATAL_ERROR "mbedTLS not found; open62541 ${GATEWAY_OPEN62541_VERSION} requires it "
                      "(install libmbedtls-dev)")
endif()

find_package(Threads REQUIRED)
add_library(open62541 STATIC "${open62541_source_SOURCE_DIR}/open62541.c")
target_include_directories(open62541 SYSTEM PUBLIC "${open62541_header_SOURCE_DIR}")
target_include_directories(open62541 SYSTEM PRIVATE "${MBEDTLS_INCLUDE_DIR}")
target_link_libraries(open62541 PUBLIC ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY}
                                       Threads::Threads)
set_target_properties(open62541 PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

if(GATEWAY_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
