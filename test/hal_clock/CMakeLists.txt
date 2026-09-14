add_executable(HalClockTest
  HalClockTest.cpp
  ${REPO_ROOT}/lib/hal/HalClock.cpp
)
target_include_directories(HalClockTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
)
target_link_libraries(HalClockTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalClockTest)
