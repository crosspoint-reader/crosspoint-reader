add_executable(SerialLineBufferTest
  SerialLineBufferTest.cpp
)

target_include_directories(SerialLineBufferTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SerialLineBufferTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerialLineBufferTest)
