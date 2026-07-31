add_executable(AutomaticSleepSyncPolicyTest
  AutomaticSleepSyncPolicyTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/AutoSleepSyncPolicy.cpp
)

target_include_directories(AutomaticSleepSyncPolicyTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(AutomaticSleepSyncPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AutomaticSleepSyncPolicyTest)
