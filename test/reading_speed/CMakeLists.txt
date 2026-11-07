add_executable(ReadingSpeedTest ReadingSpeedTest.cpp)

target_include_directories(ReadingSpeedTest PRIVATE ../../src ../../src/activities/reader)

target_link_libraries(ReadingSpeedTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingSpeedTest)
