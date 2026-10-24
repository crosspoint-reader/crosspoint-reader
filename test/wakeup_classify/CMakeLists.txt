add_executable(WakeupClassifyTest WakeupClassifyTest.cpp)

target_include_directories(WakeupClassifyTest PRIVATE ../../lib/hal)

target_link_libraries(WakeupClassifyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WakeupClassifyTest)
