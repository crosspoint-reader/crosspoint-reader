add_executable(SemVersionTest
  SemVersionTest.cpp
  ${REPO_ROOT}/src/util/SemVersionUtils.cpp
)

target_include_directories(SemVersionTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(SemVersionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SemVersionTest)
