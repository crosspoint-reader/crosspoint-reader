add_executable(DifferentialRoundingTest
  DifferentialRoundingTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DifferentialRoundingTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(DifferentialRoundingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DifferentialRoundingTest)
