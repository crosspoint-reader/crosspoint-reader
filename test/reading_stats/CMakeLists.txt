add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  ReadingStatsPresentationTest.cpp
  ${REPO_ROOT}/src/activities/network/NearbyStatsStorage.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsCodec.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsCompletionTransaction.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsEnvelope.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsModels.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsPresentation.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsStorage.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsVersionGuard.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)

target_include_directories(ReadingStatsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
