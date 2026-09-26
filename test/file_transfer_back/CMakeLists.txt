add_executable(FileTransferBackTest FileTransferBackTest.cpp)

target_include_directories(FileTransferBackTest PRIVATE ../../src)

target_link_libraries(FileTransferBackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FileTransferBackTest)
