add_executable(TocFallbackPolicyTest
  TocFallbackPolicyTest.cpp
)

target_include_directories(TocFallbackPolicyTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(TocFallbackPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TocFallbackPolicyTest)
