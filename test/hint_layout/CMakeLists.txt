add_executable(HintLayoutTest
  HintLayoutTest.cpp
)

target_include_directories(HintLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HintLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HintLayoutTest)
