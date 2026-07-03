add_executable(DictWordSelectNavigatorTest
  DictWordSelectNavigatorTest.cpp
  ${REPO_ROOT}/src/util/WordSelectNavigator.cpp
)

target_include_directories(DictWordSelectNavigatorTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # overrides GfxRenderer.h / MappedInputManager.h (device-only)
  ${REPO_ROOT}/test/stubs  # overrides HalStorage.h / Logging.h (device-only)
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(DictWordSelectNavigatorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictWordSelectNavigatorTest)
