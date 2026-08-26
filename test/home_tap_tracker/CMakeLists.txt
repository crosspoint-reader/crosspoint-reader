add_executable(HomeTapTrackerTest
  HomeTapTrackerTest.cpp
)

target_include_directories(HomeTapTrackerTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HomeTapTrackerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeTapTrackerTest)
