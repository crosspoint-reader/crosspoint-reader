add_executable(FrontlightScheduleTest
  FrontlightScheduleTest.cpp
)

target_include_directories(FrontlightScheduleTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(FrontlightScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FrontlightScheduleTest)
