add_executable(IdleGateTest
  IdleGateTest.cpp
)

target_include_directories(IdleGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(IdleGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IdleGateTest)
