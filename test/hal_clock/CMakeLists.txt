cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(hal_clock_tests CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  add_library(crosspoint_test_common INTERFACE)
  target_include_directories(crosspoint_test_common INTERFACE
    ${REPO_ROOT}
    ${REPO_ROOT}/lib
  )
  target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
  enable_testing()
  include(GoogleTest)
endif()

# These tests wrap libc clock writes, so the host's real clock is never changed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_executable(HalClockTest
    HalClockTest.cpp
    ${REPO_ROOT}/lib/hal/HalClock.cpp
  )
  target_include_directories(HalClockTest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
  target_include_directories(HalClockTest PRIVATE ${REPO_ROOT}/lib/hal)
  target_link_options(HalClockTest PRIVATE -Wl,--wrap=time -Wl,--wrap=settimeofday)
  target_link_libraries(HalClockTest PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(HalClockTest)
endif()
