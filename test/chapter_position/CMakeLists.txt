add_executable(ChapterPositionTest
  ChapterPositionTest.cpp
)

target_link_libraries(ChapterPositionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterPositionTest)
