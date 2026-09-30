add_executable(CombiningMarkAnchorTest
  CombiningMarkAnchorTest.cpp
)

target_include_directories(CombiningMarkAnchorTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(CombiningMarkAnchorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CombiningMarkAnchorTest)
