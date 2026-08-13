add_executable(LibraryTextTest
  LibraryTextTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(LibraryTextTest PRIVATE
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(LibraryTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryTextTest)
