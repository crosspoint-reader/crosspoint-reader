add_executable(ReaderTouchTest ReaderTouchTest.cpp)

target_include_directories(ReaderTouchTest PRIVATE
  stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

target_link_libraries(ReaderTouchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderTouchTest)
