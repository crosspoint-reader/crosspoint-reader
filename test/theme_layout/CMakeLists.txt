add_executable(ThemeLayoutTest
  ThemeLayoutTest.cpp
  ${REPO_ROOT}/src/components/themes/ThemeLayout.cpp
)

target_include_directories(ThemeLayoutTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

target_link_libraries(ThemeLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThemeLayoutTest)
