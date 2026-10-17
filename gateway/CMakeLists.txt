add_library(gateway_domain STATIC src/domain/telemetry_json.cpp)
target_include_directories(gateway_domain PUBLIC include)
target_link_libraries(gateway_domain PUBLIC nlohmann_json::nlohmann_json)
gateway_set_warnings(gateway_domain)

add_library(gateway_config STATIC src/config/config_loader.cpp)
target_include_directories(gateway_config PUBLIC include)
target_link_libraries(gateway_config PRIVATE yaml-cpp::yaml-cpp)
gateway_set_warnings(gateway_config)

# SDK-neutral OPC UA client abstraction (header-only; no open62541 dependency).
add_library(gateway_opcua_api INTERFACE)
target_include_directories(gateway_opcua_api INTERFACE include)
target_link_libraries(gateway_opcua_api INTERFACE gateway_domain)

# open62541 adapter. open62541 is linked PRIVATE so it cannot leak into dependants; the
# private headers under src/ are only reachable by this target (and its unit tests).
add_library(gateway_opcua_open62541 STATIC src/opcua/open62541_client.cpp src/opcua/ua_conversion.cpp)
target_include_directories(gateway_opcua_open62541 PRIVATE src)
target_link_libraries(gateway_opcua_open62541 PUBLIC gateway_opcua_api PRIVATE open62541 spdlog::spdlog)
gateway_set_warnings(gateway_opcua_open62541)

# Protocol-neutral output publisher abstraction (header-only; no MQTT dependency).
add_library(gateway_output_api INTERFACE)
target_include_directories(gateway_output_api INTERFACE include)
target_link_libraries(gateway_output_api INTERFACE gateway_domain)

# Gateway core (header-only for now; the runtime in Issue #10 adds sources).
add_library(gateway_core INTERFACE)
target_include_directories(gateway_core INTERFACE include)
target_link_libraries(gateway_core INTERFACE gateway_domain)

add_executable(gateway src/main.cpp)
target_include_directories(gateway PRIVATE include)
target_link_libraries(gateway PRIVATE spdlog::spdlog)
gateway_set_warnings(gateway)

if(GATEWAY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
