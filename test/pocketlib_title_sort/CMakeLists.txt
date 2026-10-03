# Pocket Library: where a title files on the shelf (lib/LibraryIndex/TitleSortKey.h).
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(PocketlibTitleSortTest
  TitleSortTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(PocketlibTitleSortTest PRIVATE
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(PocketlibTitleSortTest PRIVATE POCKET_LIBRARY=1)

target_link_libraries(PocketlibTitleSortTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PocketlibTitleSortTest)
