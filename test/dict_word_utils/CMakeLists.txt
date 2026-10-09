add_executable(DictWordUtilsTest
  DictWordUtilsTest.cpp
  ${REPO_ROOT}/src/util/DictWordUtils.cpp
)

target_include_directories(DictWordUtilsTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(DictWordUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictWordUtilsTest)
