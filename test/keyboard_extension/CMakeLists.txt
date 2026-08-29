add_executable(KeyboardExtensionTest
  KeyboardExtensionTest.cpp
  ${REPO_ROOT}/src/activities/util/KeyboardExtension.cpp
)

target_include_directories(KeyboardExtensionTest PRIVATE
  ${REPO_ROOT}/src/activities/util
)

target_link_libraries(KeyboardExtensionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KeyboardExtensionTest)
