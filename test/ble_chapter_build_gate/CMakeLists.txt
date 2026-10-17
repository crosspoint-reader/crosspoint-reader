add_executable(ChapterBuildGateTest ChapterBuildGateTest.cpp)

target_include_directories(ChapterBuildGateTest PRIVATE ../../src ../../lib/BlePageTurner/include
  ../../freeink-sdk/libs/network/BleKeyboardHost/include)

target_link_libraries(ChapterBuildGateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterBuildGateTest)
