add_executable(SettingsExtensionTest
  SettingsExtensionTest.cpp
  ${REPO_ROOT}/src/activities/settings/SettingsExtension.cpp
)

target_include_directories(SettingsExtensionTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(SettingsExtensionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SettingsExtensionTest)
