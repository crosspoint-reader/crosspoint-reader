add_executable(PasswordMaskingTest
  PasswordMaskingTest.cpp
)

target_include_directories(PasswordMaskingTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PasswordMaskingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PasswordMaskingTest)
