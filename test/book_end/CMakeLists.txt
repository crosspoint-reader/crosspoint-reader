add_executable(BookEndTest
  BookEndTest.cpp
)

target_include_directories(BookEndTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(BookEndTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookEndTest)
