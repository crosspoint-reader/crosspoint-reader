add_executable(IndexingProgressGateTest
  IndexingProgressGateTest.cpp
)

target_include_directories(IndexingProgressGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(IndexingProgressGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IndexingProgressGateTest)
