add_executable(SettingsCategoryGridLayoutTest
  SettingsCategoryGridLayoutTest.cpp
)

target_include_directories(SettingsCategoryGridLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SettingsCategoryGridLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SettingsCategoryGridLayoutTest)
