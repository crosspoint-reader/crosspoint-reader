add_executable(BookCacheUtilsTest
  BookCacheUtilsTest.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
)

target_include_directories(BookCacheUtilsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/util
)

target_link_libraries(BookCacheUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookCacheUtilsTest)
