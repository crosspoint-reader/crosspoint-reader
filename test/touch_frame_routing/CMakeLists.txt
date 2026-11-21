add_executable(TouchFrameRoutingTest TouchFrameRoutingTest.cpp)
target_include_directories(TouchFrameRoutingTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)
target_link_libraries(TouchFrameRoutingTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TouchFrameRoutingTest)
