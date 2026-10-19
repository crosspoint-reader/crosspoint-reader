add_executable(LaneBeforeBuildTest
  LaneBeforeBuildTest.cpp
)

target_include_directories(LaneBeforeBuildTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LaneBeforeBuildTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LaneBeforeBuildTest)
