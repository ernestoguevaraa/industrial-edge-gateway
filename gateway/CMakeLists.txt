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

# SDK-neutral acquisition API and OPC UA compatibility bridge.
add_library(gateway_acquisition_api INTERFACE)
target_include_directories(gateway_acquisition_api INTERFACE include)
target_link_libraries(gateway_acquisition_api INTERFACE gateway_domain)

add_library(gateway_acquisition_opcua STATIC src/acquisition/opcua_source.cpp)
target_include_directories(gateway_acquisition_opcua PUBLIC include)
target_link_libraries(gateway_acquisition_opcua PUBLIC gateway_acquisition_api gateway_opcua_api)
gateway_set_warnings(gateway_acquisition_opcua)

# Transport-independent Modbus data decoder. Only the header-only configuration and domain types
# are used (include path, no link dependency), so it needs no external library.
add_library(gateway_modbus_decoder STATIC src/modbus/decoder.cpp)
target_include_directories(gateway_modbus_decoder PUBLIC include)
gateway_set_warnings(gateway_modbus_decoder)

# SDK-neutral Modbus client abstraction (IModbusClient, errors, request validation).
add_library(gateway_modbus_api STATIC src/modbus/modbus_client.cpp)
target_include_directories(gateway_modbus_api PUBLIC include)
target_link_libraries(gateway_modbus_api PUBLIC gateway_modbus_decoder)
gateway_set_warnings(gateway_modbus_api)

# libmodbus-backed Modbus TCP client. libmodbus is linked PRIVATE so it cannot leak into
# dependants; the private headers under src/ are only reachable by this target and its tests.
add_library(gateway_modbus_tcp STATIC src/modbus/libmodbus_client.cpp src/modbus/libmodbus_errors.cpp)
target_include_directories(gateway_modbus_tcp PRIVATE src)
target_link_libraries(gateway_modbus_tcp PUBLIC gateway_modbus_api PRIVATE libmodbus::libmodbus)
gateway_set_warnings(gateway_modbus_tcp)

# Modbus TCP acquisition source: polls one device with an IModbusClient (LibModbusClient by
# default) on its own worker thread. One instance per configured device in the runtime.
add_library(gateway_modbus_source STATIC src/modbus/modbus_tcp_source.cpp src/modbus/poll_plan.cpp)
target_include_directories(gateway_modbus_source PRIVATE src)
target_link_libraries(gateway_modbus_source PUBLIC gateway_acquisition_api gateway_modbus_api PRIVATE gateway_modbus_tcp
                                                                                              Threads::Threads)
gateway_set_warnings(gateway_modbus_source)

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

# Gateway runtime: wires configuration, the acquisition sources, the queue and the MQTT publisher.
# Its public interface is SDK- and protocol-neutral; the OPC UA and Modbus TCP sources are private
# wiring of make_gateway_runtime().
add_library(gateway_runtime STATIC src/runtime/gateway_runtime.cpp src/runtime/runtime_factory.cpp
                                   src/runtime/telemetry_mapping.cpp)
target_include_directories(gateway_runtime PRIVATE src)
target_link_libraries(
  gateway_runtime
  PUBLIC gateway_config gateway_core gateway_acquisition_api gateway_output_api
  PRIVATE gateway_acquisition_opcua gateway_modbus_source gateway_mqtt gateway_opcua_open62541 spdlog::spdlog)
gateway_set_warnings(gateway_runtime)

add_executable(gateway src/main.cpp)
target_include_directories(gateway PRIVATE include)
target_link_libraries(gateway PRIVATE gateway_config gateway_runtime spdlog::spdlog)
gateway_set_warnings(gateway)

if(GATEWAY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
