# Pocket Library for CrossPoint — host build of the ZIM reader.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds lib/zim for macOS/Linux: the library, the `zimcat` tool and the
# GoogleTest suite (run against openZIM's zim-testing-suite files).
#
#   cmake -S lib/zim -B build/zim -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/zim -j
#   ctest --test-dir build/zim --output-on-failure
#   build/zim/zimcat wikipedia_en_all_nopic_2026-09.zim "Forbidden City"
#
# Options: -DZIM_SANITIZE=ON (ASan+UBSan), -DZIM_BUILD_TESTS=OFF,
# -DZIM_TEST_DATA_DIR=/path/to/zim-testing-suite/data (skip the download).

cmake_minimum_required(VERSION 3.16)
project(pocketlib_zim C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ZIM_BUILD_TESTS "Build the GoogleTest suite" ON)
option(ZIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(ZIM_TEST_DATA_DIR "" CACHE PATH "zim-testing-suite data directory (downloaded if empty)")

if(ZIM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

# Vendored decoders, compiled as C with their own warnings left alone.
add_library(zim_thirdparty STATIC
  src/third_party/zstd/zstddeclib.c
  src/third_party/xz/xz_crc32.c
  src/third_party/xz/xz_crc64.c
  src/third_party/xz/xz_dec_lzma2.c
  src/third_party/xz/xz_dec_stream.c
)
target_include_directories(zim_thirdparty PUBLIC src/third_party/xz)
target_compile_definitions(zim_thirdparty PRIVATE ZSTD_DISABLE_ASM=1)

add_library(zim STATIC
  src/ZimSource.cpp
  src/ZimArchive.cpp
  src/ZimDecompress.cpp
  src/ZimFold.cpp
  src/ZimTitleIndex.cpp
  src/ZimHtml.cpp
)
target_include_directories(zim PUBLIC src)
target_link_libraries(zim PRIVATE zim_thirdparty)
target_compile_options(zim PRIVATE -Wall -Wextra -pedantic)

add_library(zim_host STATIC host/PosixSource.cpp host/TitleIndexWriter.cpp)
target_include_directories(zim_host PUBLIC host)
target_link_libraries(zim_host PUBLIC zim)
target_compile_options(zim_host PRIVATE -Wall -Wextra -pedantic)

add_executable(zimcat tools/zimcat.cpp)
target_link_libraries(zimcat PRIVATE zim_host zim zim_thirdparty)
target_compile_options(zimcat PRIVATE -Wall -Wextra -pedantic)

add_executable(zimindex tools/zimindex.cpp)
target_link_libraries(zimindex PRIVATE zim_host zim zim_thirdparty)
target_compile_options(zimindex PRIVATE -Wall -Wextra -pedantic)

if(ZIM_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  if(ZIM_TEST_DATA_DIR STREQUAL "")
    FetchContent_Declare(zim_testing_suite
      GIT_REPOSITORY https://github.com/openzim/zim-testing-suite.git
      GIT_TAG 2edf72096208e60c82b6ebbe313baa552cc6af52)
    FetchContent_MakeAvailable(zim_testing_suite)
    set(ZIM_TEST_DATA_DIR "${zim_testing_suite_SOURCE_DIR}/data")
  endif()

  enable_testing()
  include(GoogleTest)
  # The firmware's own expat, built with the firmware's flags, so the HTML
  # cleaner's output is checked by the same parser that lays it out on device.
  add_library(zim_test_expat STATIC
    ../expat/xmlparse.c ../expat/xmlrole.c ../expat/xmltok.c)
  target_include_directories(zim_test_expat PUBLIC ../expat)
  target_compile_definitions(zim_test_expat PUBLIC XML_GE=0 XML_CONTEXT_BYTES=1024 HAVE_EXPAT_CONFIG_H)

  add_executable(zim_tests test/ZimArchiveTest.cpp test/SplitSourceTest.cpp test/TitleIndexTest.cpp
    test/HtmlCleanTest.cpp)
  target_link_libraries(zim_tests PRIVATE zim_host zim zim_thirdparty zim_test_expat GTest::gtest_main)
  target_compile_definitions(zim_tests PRIVATE ZIM_TEST_DATA_DIR="${ZIM_TEST_DATA_DIR}")
  target_compile_options(zim_tests PRIVATE -Wall -Wextra)
  gtest_discover_tests(zim_tests DISCOVERY_TIMEOUT 60)
endif()
