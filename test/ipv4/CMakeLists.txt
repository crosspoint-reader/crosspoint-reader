add_executable(Ipv4Test
  Ipv4Test.cpp
  ${REPO_ROOT}/src/util/Ipv4.cpp
)

target_include_directories(Ipv4Test PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(Ipv4Test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Ipv4Test)
