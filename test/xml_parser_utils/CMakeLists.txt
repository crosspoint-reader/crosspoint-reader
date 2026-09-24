add_executable(XmlParserUtilsTest
  XmlParserUtilsTest.cpp
)

target_include_directories(XmlParserUtilsTest PRIVATE
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)

target_link_libraries(XmlParserUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(XmlParserUtilsTest)
