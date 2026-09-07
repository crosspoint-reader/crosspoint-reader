add_executable(FirmwareImageIdentityTest
  FirmwareImageIdentityTest.cpp
)

target_link_libraries(FirmwareImageIdentityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareImageIdentityTest)
