add_executable(ProtectedPathsTest ProtectedPathsTest.cpp)

target_include_directories(ProtectedPathsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ProtectedPathsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProtectedPathsTest)
