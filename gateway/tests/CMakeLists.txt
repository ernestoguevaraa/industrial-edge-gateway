include(GoogleTest)

add_executable(
  gateway_tests
  config/config_loader_test.cpp
  core/bounded_queue_test.cpp
  domain/data_quality_test.cpp
  domain/telemetry_json_test.cpp
  opcua/backoff_test.cpp
  opcua/open62541_client_test.cpp
  opcua/opcua_client_mock_test.cpp
  opcua/status_code_test.cpp
  opcua/ua_conversion_test.cpp)
# The adapter's private headers (gateway/src) are visible to its unit tests only.
target_include_directories(gateway_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/gateway/src)
target_link_libraries(
  gateway_tests
  PRIVATE gateway_config
          gateway_core
          gateway_domain
          gateway_opcua_api
          gateway_opcua_open62541
          open62541
          GTest::gmock
          GTest::gtest_main)
target_compile_definitions(gateway_tests
                           PRIVATE IEG_EXAMPLE_CONFIG_PATH="${PROJECT_SOURCE_DIR}/configs/gateway.example.yaml")
gateway_set_warnings(gateway_tests)

gtest_discover_tests(gateway_tests)

# Real adapter against the real Python simulator. Needs IEG_SIMULATOR_PYTHON at run time
# (an interpreter with simulator/requirements.txt installed); skipped otherwise.
add_executable(gateway_integration_tests integration/open62541_simulator_test.cpp)
target_include_directories(gateway_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gateway_integration_tests PRIVATE gateway_opcua_open62541 GTest::gtest_main)
target_compile_definitions(gateway_integration_tests PRIVATE IEG_SIMULATOR_DIR="${PROJECT_SOURCE_DIR}/simulator")
gateway_set_warnings(gateway_integration_tests)

gtest_discover_tests(gateway_integration_tests PROPERTIES LABELS integration DISCOVERY_TIMEOUT 30)
