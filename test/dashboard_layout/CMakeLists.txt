add_executable(DashboardLayoutTest
  DashboardLayoutTest.cpp
  SectionCacheLayoutTest.cpp
  SectionCacheValidatorTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/SectionCacheValidator.cpp
)

target_include_directories(DashboardLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DashboardLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DashboardLayoutTest)
