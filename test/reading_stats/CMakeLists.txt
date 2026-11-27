add_executable(ReadingStatsRecordTest
  ReadingStatsRecordTest.cpp
  ${REPO_ROOT}/src/stats/ReadingStatsRecord.cpp
)

target_include_directories(ReadingStatsRecordTest PRIVATE
  ${REPO_ROOT}/src/stats
)

target_link_libraries(ReadingStatsRecordTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsRecordTest)
