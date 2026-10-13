include(GoogleTest)

add_executable(
  gateway_tests
  config/config_loader_test.cpp
  domain/data_quality_test.cpp
  domain/telemetry_json_test.cpp)
target_link_libraries(gateway_tests PRIVATE gateway_config gateway_domain GTest::gmock GTest::gtest_main)
target_compile_definitions(gateway_tests
                           PRIVATE IEG_EXAMPLE_CONFIG_PATH="${PROJECT_SOURCE_DIR}/configs/gateway.example.yaml")
gateway_set_warnings(gateway_tests)

gtest_discover_tests(gateway_tests)
