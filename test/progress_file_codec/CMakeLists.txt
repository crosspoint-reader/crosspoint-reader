add_executable(ProgressFileCodecTest
  ProgressFileCodecTest.cpp
)

target_include_directories(ProgressFileCodecTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ProgressFileCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressFileCodecTest)
