add_executable(LongPressRotationTest
  LongPressRotationTest.cpp
)

target_link_libraries(LongPressRotationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LongPressRotationTest)
