add_executable(ChapterBuildGateTest ChapterBuildGateTest.cpp)

target_include_directories(ChapterBuildGateTest PRIVATE ../../src ../../lib/BlePageTurner/include)

target_link_libraries(ChapterBuildGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterBuildGateTest)
