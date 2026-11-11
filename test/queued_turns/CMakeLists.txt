add_executable(QueuedTurnsTest QueuedTurnsTest.cpp)

target_include_directories(QueuedTurnsTest PRIVATE ../../src)

target_link_libraries(QueuedTurnsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QueuedTurnsTest)
