add_executable(OpdsFeedErrorTest
  OpdsFeedErrorTest.cpp
)

target_include_directories(OpdsFeedErrorTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(OpdsFeedErrorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsFeedErrorTest)
