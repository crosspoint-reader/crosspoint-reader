add_executable(DashboardLayoutTest
  DashboardLayoutTest.cpp
)

target_include_directories(DashboardLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DashboardLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardLayoutTest)
