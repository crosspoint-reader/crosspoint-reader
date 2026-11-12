add_executable(LibraryBuilderTest
  LibraryBuilderTest.cpp
  NextBookFinderTest.cpp
  ${REPO_ROOT}/src/util/NextBookFinder.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexFile.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(LibraryBuilderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/fs_helpers/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/util
)
target_link_libraries(LibraryBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LibraryBuilderTest)
