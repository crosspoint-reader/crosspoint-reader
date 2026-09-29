add_executable(BookCachePathTest
  BookCachePathTest.cpp
  ${REPO_ROOT}/lib/BookCachePath/BookCachePath.cpp
)

target_include_directories(BookCachePathTest PRIVATE
  ${REPO_ROOT}/lib/BookCachePath
)

target_link_libraries(BookCachePathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCachePathTest)
