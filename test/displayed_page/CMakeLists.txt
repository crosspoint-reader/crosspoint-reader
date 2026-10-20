add_executable(DisplayedPageTest
  DisplayedPageTest.cpp
)

target_include_directories(DisplayedPageTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(DisplayedPageTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DisplayedPageTest)
