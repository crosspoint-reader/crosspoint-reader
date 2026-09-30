add_executable(LinkNoteTitleTest LinkNoteTitleTest.cpp)

target_include_directories(LinkNoteTitleTest PRIVATE ../../src ../../lib/BlePageTurner/include)

target_link_libraries(LinkNoteTitleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LinkNoteTitleTest)
