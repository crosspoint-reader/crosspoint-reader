add_executable(RemoteTurnGateTest RemoteTurnGateTest.cpp)

target_include_directories(RemoteTurnGateTest PRIVATE ../../src)

target_link_libraries(RemoteTurnGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RemoteTurnGateTest)
