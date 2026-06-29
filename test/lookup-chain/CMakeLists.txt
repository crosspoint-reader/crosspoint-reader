add_executable(LookupChainTest LookupChainTest.cpp)

target_include_directories(LookupChainTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LookupChainTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LookupChainTest)
