include(GoogleTest)

add_executable(
  gateway_tests
  config/config_loader_test.cpp
  core/bounded_queue_test.cpp
  domain/data_quality_test.cpp
  domain/telemetry_json_test.cpp
  common/backoff_test.cpp
  opcua/open62541_client_test.cpp
  opcua/opcua_client_mock_test.cpp
  opcua/status_code_test.cpp
  mqtt/completion_registry_test.cpp
  mqtt/mqtt_publisher_test.cpp
  mqtt/payload_reason_test.cpp
  mqtt/submit_result_test.cpp
  mqtt/topic_test.cpp
  opcua/ua_conversion_test.cpp
  output/publish_result_test.cpp
  output/telemetry_publisher_mock_test.cpp)
# The adapter's private headers (gateway/src) are visible to its unit tests only.
target_include_directories(gateway_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/gateway/src)
target_link_libraries(
  gateway_tests
  PRIVATE gateway_config
          gateway_core
          gateway_domain
          gateway_mqtt
          gateway_opcua_api
          gateway_opcua_open62541
          gateway_output_api
          eclipse-paho-mqtt-c::paho-mqtt3a-static
          open62541
          GTest::gmock
          GTest::gtest_main)
target_compile_definitions(gateway_tests
                           PRIVATE IEG_EXAMPLE_CONFIG_PATH="${PROJECT_SOURCE_DIR}/configs/gateway.example.yaml")
gateway_set_warnings(gateway_tests)

gtest_discover_tests(gateway_tests)

# Real adapters against real external processes, each skipped unless its variable is set:
#  - IEG_SIMULATOR_PYTHON: interpreter with simulator/requirements.txt (OPC UA simulator)
#  - IEG_MOSQUITTO: mosquitto broker binary (MQTT publisher)
add_executable(gateway_integration_tests integration/mqtt_mosquitto_test.cpp integration/open62541_simulator_test.cpp)
target_include_directories(gateway_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gateway_integration_tests PRIVATE gateway_domain gateway_mqtt gateway_opcua_open62541
                                                        eclipse-paho-mqtt-c::paho-mqtt3a-static GTest::gtest_main)
target_compile_definitions(gateway_integration_tests PRIVATE IEG_SIMULATOR_DIR="${PROJECT_SOURCE_DIR}/simulator")
gateway_set_warnings(gateway_integration_tests)

gtest_discover_tests(gateway_integration_tests PROPERTIES LABELS integration DISCOVERY_TIMEOUT 30)
