cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(hal_storage_tests CXX)
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
  target_compile_options(crosspoint_test_common INTERFACE -Wall -Wextra -pedantic)
  enable_testing()
  include(GoogleTest)
endif()

add_executable(HalStorageTest
  HalStorageTest.cpp
  ${REPO_ROOT}/lib/hal/HalStorage.cpp
)
target_include_directories(HalStorageTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(HalStorageTest PRIVATE FREEINK_CAP_USB_MSC=0)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(HalStorageTest PRIVATE -fno-exceptions -UNDEBUG)
elseif(MSVC)
  target_compile_options(HalStorageTest PRIVATE /UNDEBUG)
endif()
target_link_libraries(HalStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalStorageTest)
