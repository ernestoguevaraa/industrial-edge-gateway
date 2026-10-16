include(GoogleTest)

add_executable(
  gateway_tests
  config/config_loader_test.cpp
  domain/data_quality_test.cpp
  domain/telemetry_json_test.cpp
  opcua/opcua_client_mock_test.cpp
  opcua/status_code_test.cpp)
target_include_directories(gateway_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gateway_tests PRIVATE gateway_config gateway_domain gateway_opcua_api GTest::gmock
                                            GTest::gtest_main)
target_compile_definitions(gateway_tests
                           PRIVATE IEG_EXAMPLE_CONFIG_PATH="${PROJECT_SOURCE_DIR}/configs/gateway.example.yaml")
gateway_set_warnings(gateway_tests)

gtest_discover_tests(gateway_tests)
