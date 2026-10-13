add_executable(gateway src/main.cpp)
target_include_directories(gateway PRIVATE include)
target_link_libraries(gateway PRIVATE spdlog::spdlog)
gateway_set_warnings(gateway)

if(GATEWAY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
