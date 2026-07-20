add_executable(TxtLineWrapTest
  TxtLineWrapTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TxtLineWrapTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TxtLineWrapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TxtLineWrapTest)
