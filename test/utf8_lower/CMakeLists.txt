add_executable(Utf8LowerTest
  Utf8LowerTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(Utf8LowerTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(Utf8LowerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Utf8LowerTest)
