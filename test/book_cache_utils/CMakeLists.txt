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

add_executable(BookFilePublishTest
  BookFilePublishTest.cpp
  UploadPathGuardTest.cpp
  ${REPO_ROOT}/src/network/UploadPathGuard.cpp
  ${REPO_ROOT}/src/util/BookPathMoveUtils.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
)

target_include_directories(BookFilePublishTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/src/util
)

target_link_libraries(BookFilePublishTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookFilePublishTest)
