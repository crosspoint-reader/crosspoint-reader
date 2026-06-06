add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ${REPO_ROOT}/lib/ReadingStats/ReadingStats.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${REPO_ROOT}/lib/ReadingStats
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
