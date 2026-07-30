add_executable(OpdsFilenameTest
  OpdsFilenameTest.cpp
  ${REPO_ROOT}/src/util/OpdsFilename.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(OpdsFilenameTest PRIVATE
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(OpdsFilenameTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsFilenameTest)
