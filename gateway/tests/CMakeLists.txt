include(GoogleTest)

add_executable(gateway_tests domain/data_quality_test.cpp domain/telemetry_json_test.cpp)
target_link_libraries(gateway_tests PRIVATE gateway_domain GTest::gtest_main)
gateway_set_warnings(gateway_tests)

gtest_discover_tests(gateway_tests)
