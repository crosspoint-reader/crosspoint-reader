add_executable(WakeHoldTest WakeHoldTest.cpp)

target_include_directories(WakeHoldTest PRIVATE ../../src)

target_link_libraries(WakeHoldTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WakeHoldTest)
