add_executable(UsbLinkDebounceTest
  UsbLinkDebounceTest.cpp
)

target_link_libraries(UsbLinkDebounceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UsbLinkDebounceTest)
