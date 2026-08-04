add_executable(KOReaderDocumentIdParserTest
  KOReaderDocumentIdParserTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/OriginalDocumentIdParser.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(KOReaderDocumentIdParserTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(KOReaderDocumentIdParserTest PRIVATE
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_link_libraries(KOReaderDocumentIdParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderDocumentIdParserTest)
