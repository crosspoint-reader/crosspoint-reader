add_executable(NightScheduleTest
  NightScheduleTest.cpp
  ${REPO_ROOT}/src/util/NightSchedule.cpp
)

target_include_directories(NightScheduleTest PRIVATE ../../src)

target_link_libraries(NightScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NightScheduleTest)
