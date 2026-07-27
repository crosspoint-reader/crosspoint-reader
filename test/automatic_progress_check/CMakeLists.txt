add_executable(AutomaticProgressCheckPolicyTest
  AutomaticProgressCheckPolicyTest.cpp
)

target_include_directories(AutomaticProgressCheckPolicyTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(AutomaticProgressCheckPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AutomaticProgressCheckPolicyTest)
