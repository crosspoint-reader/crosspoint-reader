add_executable(ImageSettingsInputTest
  ImageSettingsInputTest.cpp
)

target_link_libraries(ImageSettingsInputTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImageSettingsInputTest)
