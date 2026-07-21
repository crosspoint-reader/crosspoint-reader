add_executable(ProgressFileCodecTest
  ProgressFileCodecTest.cpp
  ProgressFilePersistenceTest.cpp
)

target_include_directories(ProgressFileCodecTest PRIVATE
  ${REPO_ROOT}/test/reading_stats/stubs
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ProgressFileCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressFileCodecTest)
