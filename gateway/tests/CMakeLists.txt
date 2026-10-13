include(GoogleTest)

add_executable(gateway_tests placeholder_test.cpp)
target_link_libraries(gateway_tests PRIVATE GTest::gtest_main yaml-cpp::yaml-cpp nlohmann_json::nlohmann_json)
gateway_set_warnings(gateway_tests)

gtest_discover_tests(gateway_tests)
