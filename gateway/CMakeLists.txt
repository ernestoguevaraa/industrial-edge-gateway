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

add_executable(gateway src/main.cpp)
target_include_directories(gateway PRIVATE include)
target_link_libraries(gateway PRIVATE spdlog::spdlog)
gateway_set_warnings(gateway)

if(GATEWAY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
