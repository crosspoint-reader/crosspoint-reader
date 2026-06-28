cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

FetchContent_Declare(
  espressif_tlsf_src
  GIT_REPOSITORY https://github.com/espressif/tlsf.git
  GIT_TAG 2867f6883a12920b1969ff9624c0ab0e4185c2ce
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_GetProperties(espressif_tlsf_src)
if(NOT espressif_tlsf_src_POPULATED)
  FetchContent_Populate(espressif_tlsf_src)
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

add_library(espressif_tlsf STATIC
  ${espressif_tlsf_src_SOURCE_DIR}/tlsf.c
)
target_include_directories(espressif_tlsf PUBLIC
  ${espressif_tlsf_src_SOURCE_DIR}
  ${espressif_tlsf_src_SOURCE_DIR}/include
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(simulator_heap)
