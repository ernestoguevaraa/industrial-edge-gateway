# Strict warnings for project targets only; third-party code is pulled in as SYSTEM.
function(gateway_set_warnings target)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(WARNING "Strict warnings are only configured for GCC and Clang")
    return()
  endif()

  set(warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Wunused
      -Woverloaded-virtual
      -Wconversion
      -Wsign-conversion
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(
      APPEND
      warnings
      -Wmisleading-indentation
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
      -Wuseless-cast)
  endif()

  if(GATEWAY_WARNINGS_AS_ERRORS)
    list(APPEND warnings -Werror)
  endif()

  target_compile_options(${target} PRIVATE ${warnings})
endfunction()
