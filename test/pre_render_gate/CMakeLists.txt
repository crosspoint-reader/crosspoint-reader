add_executable(PreRenderGateTest
  PreRenderGateTest.cpp
)

target_include_directories(PreRenderGateTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PreRenderGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PreRenderGateTest)
