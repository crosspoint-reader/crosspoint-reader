add_executable(LibraryBuilderTest
  LibraryBuilderTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexFile.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(LibraryBuilderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(LibraryBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LibraryBuilderTest)
