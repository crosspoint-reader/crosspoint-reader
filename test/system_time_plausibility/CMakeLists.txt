add_executable(SystemTimePlausibilityTest
  SystemTimePlausibilityTest.cpp
)

target_include_directories(SystemTimePlausibilityTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(SystemTimePlausibilityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SystemTimePlausibilityTest)
