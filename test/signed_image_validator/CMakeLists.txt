add_executable(SignedImageValidatorTest
  SignedImageValidatorTest.cpp
)

target_link_libraries(SignedImageValidatorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SignedImageValidatorTest)
