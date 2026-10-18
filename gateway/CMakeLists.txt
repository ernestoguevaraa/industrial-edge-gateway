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

# MQTT 5 publisher. Paho is linked PRIVATE so it cannot leak into dependants; the private
# headers under src/ are only reachable by this target (and its unit tests).
add_library(gateway_mqtt STATIC src/mqtt/mqtt_publisher.cpp src/mqtt/payload.cpp src/mqtt/reason_codes.cpp
                                src/mqtt/submit_result.cpp src/mqtt/topic.cpp src/mqtt/paho_trace.cpp)
target_include_directories(gateway_mqtt PRIVATE src)
target_link_libraries(gateway_mqtt PUBLIC gateway_output_api PRIVATE eclipse-paho-mqtt-c::paho-mqtt3a-static
                                                                     spdlog::spdlog)
gateway_set_warnings(gateway_mqtt)

# Gateway core primitives (header-only).
add_library(gateway_core INTERFACE)
target_include_directories(gateway_core INTERFACE include)
target_link_libraries(gateway_core INTERFACE gateway_domain)

# Gateway runtime: wires configuration, the OPC UA adapter, the queue and the MQTT publisher.
add_library(gateway_runtime STATIC src/runtime/gateway_runtime.cpp src/runtime/runtime_factory.cpp
                                   src/runtime/telemetry_mapping.cpp)
target_include_directories(gateway_runtime PRIVATE src)
target_link_libraries(
  gateway_runtime
  PUBLIC gateway_config gateway_core gateway_opcua_api gateway_output_api
  PRIVATE gateway_mqtt gateway_opcua_open62541 spdlog::spdlog)
gateway_set_warnings(gateway_runtime)

add_executable(gateway src/main.cpp)
target_include_directories(gateway PRIVATE include)
target_link_libraries(gateway PRIVATE gateway_config gateway_runtime spdlog::spdlog)
gateway_set_warnings(gateway)

if(GATEWAY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
