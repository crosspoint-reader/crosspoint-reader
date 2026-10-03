add_executable(CjkLineBreakTest
  CjkLineBreakTest.cpp
)

target_include_directories(CjkLineBreakTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(CjkLineBreakTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CjkLineBreakTest)
