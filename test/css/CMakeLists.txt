add_executable(CssLengthTest
  CssLengthTest.cpp
)

# Keep the original transposed spelling working for manual builds.
add_custom_target(CssLenghtTest DEPENDS CssLengthTest)

target_link_libraries(CssLengthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssLengthTest)
