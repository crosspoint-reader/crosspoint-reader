add_executable(SerialCommandTest
  SerialCommandTest.cpp
  SerialTransferTest.cpp
)

target_include_directories(SerialCommandTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/src/util
)

target_link_libraries(SerialCommandTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerialCommandTest)
