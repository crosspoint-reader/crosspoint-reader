add_executable(PluginBundleInstallDirTest
  PluginBundleInstallDirTest.cpp
  ${REPO_ROOT}/src/util/PluginLocations.cpp
)
target_include_directories(PluginBundleInstallDirTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)
target_link_libraries(PluginBundleInstallDirTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginBundleInstallDirTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BUNDLE_BODY ${CMAKE_CURRENT_BINARY_DIR}/DownloadBundleBody.inc)
add_custom_command(OUTPUT ${BUNDLE_BODY}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/extract_bundle.py
          ${REPO_ROOT}/src/activities/plugins/PluginCatalogActivity.cpp ${BUNDLE_BODY}
  DEPENDS ${REPO_ROOT}/src/activities/plugins/PluginCatalogActivity.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/extract_bundle.py
  VERBATIM
)
add_executable(PluginBundleFlowTest
  PluginBundleFlowTest.cpp
  ${BUNDLE_BODY}
  ${REPO_ROOT}/src/util/PluginLocations.cpp
)
target_include_directories(PluginBundleFlowTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_BINARY_DIR}
  ${REPO_ROOT}/src
)
target_link_libraries(PluginBundleFlowTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginBundleFlowTest)
