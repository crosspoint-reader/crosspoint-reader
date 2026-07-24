add_executable(ReaderGestureTest ReaderGestureTest.cpp)

target_include_directories(ReaderGestureTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderGestureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderGestureTest)
