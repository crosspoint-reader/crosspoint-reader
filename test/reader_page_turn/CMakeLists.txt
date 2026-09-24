add_executable(PendingPageTurnTest PendingPageTurnTest.cpp)

target_include_directories(PendingPageTurnTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PendingPageTurnTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PendingPageTurnTest)
