add_executable(BootRecoveryPolicyTest
  BootRecoveryPolicyTest.cpp
  ${REPO_ROOT}/lib/BootRecovery/BootRecoveryPolicy.cpp
)

target_include_directories(BootRecoveryPolicyTest PRIVATE
  ${REPO_ROOT}/lib/BootRecovery
)
target_link_libraries(BootRecoveryPolicyTest PRIVATE
  GTest::gtest_main
  crosspoint_test_common
)

gtest_discover_tests(BootRecoveryPolicyTest)
