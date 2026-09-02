add_executable(PluginManifestClassificationTest
  PluginManifestClassificationTest.cpp
)

target_include_directories(PluginManifestClassificationTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PluginManifestClassificationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PluginManifestClassificationTest)
