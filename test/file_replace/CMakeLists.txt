add_executable(FileReplaceTest
  FileReplaceTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FileReplace.cpp
)

target_include_directories(FileReplaceTest PRIVATE
  # In-memory HalStorage.h with FAT's case-blind names; shadows the test/shims stub.
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

target_link_libraries(FileReplaceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FileReplaceTest)
