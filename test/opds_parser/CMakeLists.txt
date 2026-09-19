add_executable(OpdsParserTest
  OpdsParserTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp

  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(OpdsParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/OpdsParser

  ${REPO_ROOT}/lib/XmlParserUtils
)

target_compile_definitions(OpdsParserTest PRIVATE
  XML_CONTEXT_BYTES=1024
  XML_GE=0
  TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/"
)

target_link_libraries(OpdsParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsParserTest)
