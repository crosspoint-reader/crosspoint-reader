add_executable(BookDataMoveTest
  BookDataMoveTest.cpp
  ${REPO_ROOT}/src/util/BookDataMove.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/util/BookmarkUtil.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(BookDataMoveTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/fs_helpers/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src
)
target_link_libraries(BookDataMoveTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookDataMoveTest)
