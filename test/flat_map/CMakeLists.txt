add_executable(FlatMapTest
  FlatMapTest.cpp
)

target_include_directories(FlatMapTest PRIVATE
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(FlatMapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FlatMapTest)
