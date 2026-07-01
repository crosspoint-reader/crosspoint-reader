add_executable(DictLayoutTest
  DictLayoutTest.cpp
  ${REPO_ROOT}/src/util/DictLayout.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DictLayoutTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/DictHtmlRenderer
)

target_link_libraries(DictLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictLayoutTest)
