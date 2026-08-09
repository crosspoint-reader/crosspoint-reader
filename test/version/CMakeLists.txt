add_executable(VersionTest
  VersionTest.cpp
  ${REPO_ROOT}/src/util/VersionUtils.cpp
)

target_include_directories(VersionTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(VersionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VersionTest)
