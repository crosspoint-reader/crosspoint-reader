add_executable(SideHintLayoutTest
  SideHintLayoutTest.cpp
)

target_include_directories(SideHintLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SideHintLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SideHintLayoutTest)
