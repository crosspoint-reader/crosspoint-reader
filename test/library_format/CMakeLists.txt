add_executable(LibraryFormatTest
  LibraryFormatTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
)

target_include_directories(LibraryFormatTest PRIVATE
  ${REPO_ROOT}/lib/LibraryIndex
)

target_link_libraries(LibraryFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryFormatTest)
